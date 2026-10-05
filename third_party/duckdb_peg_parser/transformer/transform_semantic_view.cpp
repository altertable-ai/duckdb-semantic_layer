#include "transformer/peg_transformer.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "parsed_semantic_view.hpp"
#include "semantic_lowering.hpp"

namespace duckdb {
namespace semantic_views_peg {

// One transform per rule of grammar/statements/semantic_view.gram, registered with the factory. Children are
// addressed by their position in the rule's sequence (noted above each transform). Statements are lowered onto the
// internal table functions of semantic_lowering.hpp.

namespace {

ListParseResult &List(optional_ptr<ParseResult> parse_result) {
	return parse_result->Cast<ListParseResult>();
}

//! The alternative matched by a rule whose body is an ordered choice
optional_ptr<ParseResult> Choice(optional_ptr<ParseResult> parse_result) {
	return List(parse_result).Child<ChoiceParseResult>(0).result;
}

bool Present(ListParseResult &list, idx_t index) {
	return list.Child<OptionalParseResult>(index).HasResult();
}

optional_ptr<ParseResult> Optional(ListParseResult &list, idx_t index) {
	return list.Child<OptionalParseResult>(index).optional_result;
}

//! The elements of an `X*` (optional repeat) or `X+` (repeat) child
vector<optional_ptr<ParseResult>> Repeated(optional_ptr<ParseResult> parse_result) {
	if (parse_result->type == ParseResultType::OPTIONAL) {
		auto &optional = parse_result->Cast<OptionalParseResult>();
		if (!optional.HasResult()) {
			return {};
		}
		parse_result = optional.optional_result;
	}
	return parse_result->Cast<RepeatParseResult>().children;
}

//! The elements of a `Parens(List(X))` child
vector<optional_ptr<ParseResult>> ParenthesizedList(optional_ptr<ParseResult> parse_result) {
	return PEGTransformerFactory::ExtractParseResultsFromList(
	    PEGTransformerFactory::ExtractResultFromParens(parse_result));
}

template <class T>
vector<T> TransformAll(PEGTransformer &transformer, const vector<optional_ptr<ParseResult>> &parse_results) {
	vector<T> result;
	for (auto &parse_result : parse_results) {
		result.push_back(transformer.Transform<T>(parse_result));
	}
	return result;
}

//! The keyword matched by a rule of keyword alternatives ('PRIVATE' / 'PUBLIC', ...)
string Keyword(optional_ptr<ParseResult> parse_result) {
	return StringUtil::Upper(Choice(parse_result)->Cast<KeywordParseResult>().keyword);
}

SemanticMemberKind MemberKind(const string &keyword) {
	auto upper = StringUtil::Upper(keyword);
	if (upper == "DIMENSIONS") {
		return SemanticMemberKind::DIMENSION;
	}
	if (upper == "METRICS") {
		return SemanticMemberKind::METRIC;
	}
	D_ASSERT(upper == "FACTS");
	return SemanticMemberKind::FACT;
}

void ApplyAlias(PEGTransformer &transformer, ListParseResult &list, idx_t index, TableRef &ref) {
	if (Present(list, index)) {
		auto alias = transformer.Transform<TableAlias>(Optional(list, index));
		ref.alias = alias.name;
		ref.column_name_alias = alias.column_name_alias;
	}
}

//===--------------------------------------------------------------------===//
// Shared clauses
//===--------------------------------------------------------------------===//
// Parens(List(ColId))
vector<string> TransformSemanticColumnList(PEGTransformer &transformer, optional_ptr<ParseResult> parse_result) {
	return TransformAll<string>(transformer, ParenthesizedList(List(parse_result).GetChild(0)));
}

// 'WITH'? 'SYNONYMS' '='? Parens(List(SemanticString))
vector<string> TransformSemanticWithSynonyms(PEGTransformer &transformer, optional_ptr<ParseResult> parse_result) {
	return TransformAll<string>(transformer, ParenthesizedList(List(parse_result).GetChild(3)));
}

// 'COMMENT' '=' SemanticString
string TransformSemanticComment(PEGTransformer &transformer, optional_ptr<ParseResult> parse_result) {
	return transformer.Transform<string>(List(parse_result), 2);
}

// StringLiteral+ (adjacent string literals are concatenated, as in DuckDB's SQL parser)
string TransformSemanticString(PEGTransformer &transformer, optional_ptr<ParseResult> parse_result) {
	string result;
	for (auto &literal : Repeated(List(parse_result).GetChild(0))) {
		result += transformer.Transform<string>(literal);
	}
	return result;
}

// 'WITH'? 'TAG' Parens(List(SemanticTagEntry))
vector<ParsedSemanticTag> TransformSemanticTag(PEGTransformer &transformer, optional_ptr<ParseResult> parse_result) {
	return TransformAll<ParsedSemanticTag>(transformer, ParenthesizedList(List(parse_result).GetChild(2)));
}

// SemanticDottedName '=' SemanticString
ParsedSemanticTag TransformSemanticTagEntry(PEGTransformer &transformer, optional_ptr<ParseResult> parse_result) {
	auto &list = List(parse_result);
	ParsedSemanticTag tag;
	tag.name = StringUtil::Join(transformer.Transform<vector<string>>(list, 0), ".");
	tag.value = transformer.Transform<string>(list, 2);
	return tag;
}

// ColId ('.' ColLabel)*
vector<string> TransformSemanticDottedName(PEGTransformer &transformer, optional_ptr<ParseResult> parse_result) {
	auto &list = List(parse_result);
	vector<string> parts {transformer.Transform<string>(list, 0)};
	for (auto &part : Repeated(list.GetChild(1))) {
		parts.push_back(transformer.Transform<string>(List(part), 1));
	}
	return parts;
}

// ColId 'AS' (SemanticTableAliasAs, SemanticRelationshipName) / ColId (SemanticConstraintName)
string TransformSemanticName(PEGTransformer &transformer, optional_ptr<ParseResult> parse_result) {
	return transformer.Transform<string>(List(parse_result), 0);
}

//===--------------------------------------------------------------------===//
// TABLES
//===--------------------------------------------------------------------===//
// 'TABLES' Parens(List(SemanticLogicalTable))
vector<ParsedSemanticTable> TransformSemanticTablesClause(PEGTransformer &transformer,
                                                          optional_ptr<ParseResult> parse_result) {
	return TransformAll<ParsedSemanticTable>(transformer, ParenthesizedList(List(parse_result).GetChild(1)));
}

// SemanticTableAliasAs? SemanticTableSource SemanticTableOption*
ParsedSemanticTable TransformSemanticLogicalTable(PEGTransformer &transformer, optional_ptr<ParseResult> parse_result) {
	auto &list = List(parse_result);
	ParsedSemanticTable table;
	transformer.TransformOptional<string>(list, 0, table.alias);
	table.source = transformer.Transform<unique_ptr<TableRef>>(list, 1);
	for (auto &option_node : Repeated(list.GetChild(2))) {
		auto option = Choice(option_node);
		if (option->name == "SemanticPrimaryKey") {
			// 'PRIMARY' 'KEY' SemanticColumnList
			if (!table.primary_key.empty()) {
				throw ParserException("semantic view: logical table has more than one PRIMARY KEY");
			}
			table.primary_key = transformer.Transform<vector<string>>(List(option), 2);
		} else if (option->name == "SemanticUnique") {
			// 'UNIQUE' SemanticColumnList
			table.unique_keys.push_back(transformer.Transform<vector<string>>(List(option), 1));
		} else if (option->name == "SemanticRangeConstraint") {
			table.range_constraints.push_back(transformer.Transform<ParsedSemanticRangeConstraint>(option));
		} else if (option->name == "SemanticWithSynonyms") {
			table.synonyms = transformer.Transform<vector<string>>(option);
		} else if (option->name == "SemanticComment") {
			table.comment = transformer.Transform<string>(option);
		} else {
			table.tags = transformer.Transform<vector<ParsedSemanticTag>>(option);
		}
	}
	return table;
}

// SemanticParensSelect / BaseTableName
unique_ptr<TableRef> TransformSemanticTableSource(PEGTransformer &transformer, optional_ptr<ParseResult> parse_result) {
	auto source = Choice(parse_result);
	if (source->name == "BaseTableName") {
		return transformer.Transform<unique_ptr<BaseTableRef>>(source);
	}
	// Parens(SelectStatementInternal)
	auto select = PEGTransformerFactory::ExtractResultFromParens(List(source).GetChild(0));
	return make_uniq<SubqueryRef>(transformer.Transform<unique_ptr<SelectStatement>>(select));
}

// 'CONSTRAINT' SemanticConstraintName? 'DISTINCT' 'RANGE' 'BETWEEN' ColId 'AND' ColId 'EXCLUSIVE'
ParsedSemanticRangeConstraint TransformSemanticRangeConstraint(PEGTransformer &transformer,
                                                               optional_ptr<ParseResult> parse_result) {
	auto &list = List(parse_result);
	ParsedSemanticRangeConstraint constraint;
	transformer.TransformOptional<string>(list, 1, constraint.name);
	constraint.start_column = transformer.Transform<string>(list, 5);
	constraint.end_column = transformer.Transform<string>(list, 7);
	return constraint;
}

//===--------------------------------------------------------------------===//
// RELATIONSHIPS
//===--------------------------------------------------------------------===//
// 'RELATIONSHIPS' Parens(List(SemanticRelationship))
vector<ParsedSemanticRelationship> TransformSemanticRelationshipsClause(PEGTransformer &transformer,
                                                                       optional_ptr<ParseResult> parse_result) {
	return TransformAll<ParsedSemanticRelationship>(transformer, ParenthesizedList(List(parse_result).GetChild(1)));
}

// SemanticRelationshipName? ColId SemanticColumnList 'REFERENCES' ColId SemanticReferencedColumns?
ParsedSemanticRelationship TransformSemanticRelationship(PEGTransformer &transformer,
                                                         optional_ptr<ParseResult> parse_result) {
	auto &list = List(parse_result);
	ParsedSemanticRelationship rel;
	transformer.TransformOptional<string>(list, 0, rel.name);
	rel.from_table = transformer.Transform<string>(list, 1);
	rel.from_columns = transformer.Transform<vector<string>>(list, 2);
	rel.to_table = transformer.Transform<string>(list, 4);
	if (!Present(list, 5)) {
		return rel;
	}
	// Parens(SemanticRangeReference / List(SemanticReferencedColumn))
	auto referenced = PEGTransformerFactory::ExtractResultFromParens(List(Optional(list, 5)).GetChild(0));
	auto spec = Choice(referenced);
	if (spec->name == "SemanticRangeReference") {
		// 'BETWEEN' ColId 'AND' ColId 'EXCLUSIVE'
		rel.is_range = true;
		rel.range_start = transformer.Transform<string>(List(spec), 1);
		rel.range_end = transformer.Transform<string>(List(spec), 3);
		return rel;
	}
	auto columns = PEGTransformerFactory::ExtractParseResultsFromList(List(spec).GetChild(0));
	for (idx_t i = 0; i < columns.size(); i++) {
		// SemanticAsof? ColId
		auto &column = List(columns[i]);
		if (Present(column, 0)) {
			if (rel.asof_index != DConstants::INVALID_INDEX) {
				throw ParserException("semantic view: at most one ASOF column per relationship");
			}
			rel.asof_index = i;
		}
		rel.to_columns.push_back(transformer.Transform<string>(column, 1));
	}
	return rel;
}

//===--------------------------------------------------------------------===//
// FACTS / DIMENSIONS / METRICS
//===--------------------------------------------------------------------===//
// 'FACTS' / 'DIMENSIONS' / 'METRICS' Parens(List(SemanticMember))
vector<ParsedSemanticMember> TransformSemanticMemberClause(PEGTransformer &transformer,
                                                           optional_ptr<ParseResult> parse_result) {
	auto &list = List(parse_result);
	auto kind = MemberKind(list.Child<KeywordParseResult>(0).keyword);
	auto members = TransformAll<ParsedSemanticMember>(transformer, ParenthesizedList(list.GetChild(1)));
	for (auto &member : members) {
		member.kind = kind;
		if (member.is_private && kind == SemanticMemberKind::DIMENSION) {
			throw ParserException("semantic view: dimensions cannot be PRIVATE");
		}
		if (kind != SemanticMemberKind::METRIC && member.filter) {
			throw ParserException("semantic view: FILTER and OFFSET are only allowed on metrics ('%s')",
			                      member.FullName());
		}
		if (member.table.empty() && kind != SemanticMemberKind::METRIC) {
			throw ParserException("semantic view: %s '%s' must be qualified with its logical table (table.name)",
			                      StringUtil::Lower(SemanticMemberKindToString(kind)), member.name);
		}
	}
	return members;
}

// SemanticVisibility? SemanticMemberName SemanticLabels? SemanticUsing? SemanticNonAdditive? SemanticMetricFilter? 'AS'
// Expression SemanticMemberOption*
ParsedSemanticMember TransformSemanticMember(PEGTransformer &transformer, optional_ptr<ParseResult> parse_result) {
	auto &list = List(parse_result);
	ParsedSemanticMember member;
	member.is_private = Present(list, 0) && Keyword(Optional(list, 0)) == "PRIVATE";
	auto name = transformer.Transform<vector<string>>(list, 1);
	member.name = name.back();
	if (name.size() == 2) {
		member.table = name[0];
	}
	if (Present(list, 2)) {
		// 'LABELS' '=' Parens(List(ColId))
		member.labels = TransformAll<string>(transformer, ParenthesizedList(List(Optional(list, 2)).GetChild(2)));
	}
	if (Present(list, 3)) {
		// 'USING' Parens(List(ColId))
		member.using_relationships =
		    TransformAll<string>(transformer, ParenthesizedList(List(Optional(list, 3)).GetChild(1)));
	}
	if (Present(list, 4)) {
		// 'NON' 'ADDITIVE' 'BY' Parens(List(OrderByExpression))
		member.non_additive_by =
		    TransformAll<OrderByNode>(transformer, ParenthesizedList(List(Optional(list, 4)).GetChild(3)));
	}
	if (Present(list, 5)) {
		// 'FILTER' Parens('WHERE' Expression)
		auto body = PEGTransformerFactory::ExtractResultFromParens(List(Optional(list, 5)).GetChild(1));
		member.filter = transformer.Transform<unique_ptr<ParsedExpression>>(List(body), 1);
	}
	member.expression = transformer.Transform<unique_ptr<ParsedExpression>>(list, 7);
	for (auto &option_node : Repeated(list.GetChild(8))) {
		auto option = Choice(option_node);
		auto &option_list = List(option);
		if (option->name == "SemanticWithSynonyms") {
			member.synonyms = transformer.Transform<vector<string>>(option);
		} else if (option->name == "SemanticComment") {
			member.comment = transformer.Transform<string>(option);
		} else if (option->name == "SemanticTag") {
			member.tags = transformer.Transform<vector<ParsedSemanticTag>>(option);
		} else if (option->name == "SemanticSampleValues") {
			// 'SAMPLE_VALUES' Parens(List(SemanticString))
			member.sample_values = TransformAll<string>(transformer, ParenthesizedList(option_list.GetChild(1)));
		} else if (option->name == "SemanticIsEnum") {
			member.is_enum = true;
		} else {
			// 'WITH' 'CORTEX' 'SEARCH' 'SERVICE' SemanticDottedName ('USING' ColId)?
			member.cortex_search_service = StringUtil::Join(transformer.Transform<vector<string>>(option_list, 4), ".");
			if (Present(option_list, 5)) {
				member.cortex_search_column = transformer.Transform<string>(List(Optional(option_list, 5)), 1);
			}
		}
	}
	return member;
}

// (ColId '.' ColLabel) / ColId
vector<string> TransformSemanticMemberName(PEGTransformer &transformer, optional_ptr<ParseResult> parse_result) {
	auto name = Choice(parse_result);
	if (name->name == "SemanticQualifiedMemberName") {
		return {transformer.Transform<string>(List(name), 0), transformer.Transform<string>(List(name), 2)};
	}
	return {transformer.Transform<string>(name)};
}

//===--------------------------------------------------------------------===//
// CREATE SEMANTIC VIEW
//===--------------------------------------------------------------------===//
// ColId 'AS' Parens(SemanticVerifiedQueryProperty+)
ParsedSemanticVerifiedQuery TransformSemanticVerifiedQuery(PEGTransformer &transformer,
                                                           optional_ptr<ParseResult> parse_result) {
	auto &list = List(parse_result);
	ParsedSemanticVerifiedQuery query;
	query.name = transformer.Transform<string>(list, 0);
	for (auto &property_node : Repeated(PEGTransformerFactory::ExtractResultFromParens(list.GetChild(2)))) {
		// each property is <keyword> <value>
		auto property = Choice(property_node);
		auto &property_list = List(property);
		if (property->name == "SemanticVqQuestion") {
			query.question = transformer.Transform<string>(property_list, 1);
		} else if (property->name == "SemanticVqVerifiedAt") {
			query.verified_at = transformer.Transform<unique_ptr<ParsedExpression>>(property_list, 1)->ToString();
		} else if (property->name == "SemanticVqOnboarding") {
			query.onboarding_question = Keyword(property_list.GetChild(1)) == "TRUE";
		} else if (property->name == "SemanticVqVerifiedBy") {
			query.verified_by = transformer.Transform<string>(property_list, 1);
		} else {
			query.sql = transformer.Transform<string>(property_list, 1);
		}
	}
	return query;
}

void ApplyViewProperty(PEGTransformer &transformer, optional_ptr<ParseResult> property, ParsedSemanticView &view) {
	auto &list = List(property);
	if (property->name == "SemanticComment") {
		view.comment = transformer.Transform<string>(property);
	} else if (property->name == "SemanticMaxStaleness") {
		// 'MAX_STALENESS' '=' NumberLiteral
		auto value = transformer.Transform<unique_ptr<ParsedExpression>>(list, 2);
		view.has_max_staleness = true;
		view.max_staleness = value->Cast<ConstantExpression>().value.GetValue<int64_t>();
	} else if (property->name == "SemanticAiSqlGeneration") {
		view.ai_sql_generation = transformer.Transform<string>(list, 1);
	} else if (property->name == "SemanticAiQuestionCategorization") {
		view.ai_question_categorization = transformer.Transform<string>(list, 1);
	} else if (property->name == "SemanticAiVerifiedQueries") {
		// 'AI_VERIFIED_QUERIES' Parens(List(SemanticVerifiedQuery))
		view.verified_queries =
		    TransformAll<ParsedSemanticVerifiedQuery>(transformer, ParenthesizedList(list.GetChild(1)));
	} else if (property->name == "SemanticTag") {
		view.tags = transformer.Transform<vector<ParsedSemanticTag>>(property);
	} else {
		view.copy_grants = true;
	}
}

// SemanticTablesClause SemanticRelationshipsClause? SemanticFactsClause? SemanticDimensionsClause?
// SemanticMetricsClause? SemanticViewProperty*
unique_ptr<ParsedSemanticView> TransformSemanticViewDefinitionBody(PEGTransformer &transformer,
                                                                   optional_ptr<ParseResult> parse_result) {
	auto &list = List(parse_result);
	auto view = make_uniq<ParsedSemanticView>();
	view->tables = transformer.Transform<vector<ParsedSemanticTable>>(list, 0);
	transformer.TransformOptional(list, 1, view->relationships);
	transformer.TransformOptional(list, 2, view->facts);
	transformer.TransformOptional(list, 3, view->dimensions);
	transformer.TransformOptional(list, 4, view->metrics);
	for (auto &property : Repeated(list.GetChild(5))) {
		ApplyViewProperty(transformer, Choice(property), *view);
	}
	return view;
}

// 'CREATE' ('OR' ('REPLACE' / 'ALTER'))? 'SEMANTIC' 'VIEW' IfNotExists? QualifiedName SemanticViewDefinitionBody
unique_ptr<SQLStatement> TransformCreateSemanticViewStmt(PEGTransformer &transformer,
                                                         optional_ptr<ParseResult> parse_result) {
	auto &list = List(parse_result);
	auto or_replace = Present(list, 1);
	auto if_not_exists = Present(list, 4);
	if (or_replace && if_not_exists) {
		throw ParserException("CREATE OR REPLACE SEMANTIC VIEW cannot be combined with IF NOT EXISTS");
	}
	auto view = transformer.Transform<unique_ptr<ParsedSemanticView>>(list, 6);
	view->name = transformer.Transform<QualifiedName>(list, 5);
	auto on_conflict = or_replace      ? OnCreateConflict::REPLACE_ON_CONFLICT
	                   : if_not_exists ? OnCreateConflict::IGNORE_ON_CONFLICT
	                                   : OnCreateConflict::ERROR_ON_CONFLICT;
	return SemanticLowering::SelectAll(SemanticLowering::Create(*view, on_conflict));
}

//===--------------------------------------------------------------------===//
// DROP / ALTER / DESCRIBE / SHOW
//===--------------------------------------------------------------------===//
// 'DROP' 'SEMANTIC' 'VIEW' IfExists? QualifiedName
unique_ptr<SQLStatement> TransformDropSemanticViewStmt(PEGTransformer &transformer,
                                                       optional_ptr<ParseResult> parse_result) {
	auto &list = List(parse_result);
	auto name = transformer.Transform<QualifiedName>(list, 4);
	return SemanticLowering::SelectAll(SemanticLowering::Drop(name, Present(list, 3)));
}

void TransformQueryPieces(PEGTransformer &transformer, optional_ptr<ParseResult> pieces, ParsedSemanticQuery &query);

SemanticRefreshMode RefreshMode(const string &keyword) {
	if (keyword == "FULL") {
		return SemanticRefreshMode::FULL;
	}
	if (keyword == "INCREMENTAL") {
		return SemanticRefreshMode::INCREMENTAL;
	}
	return SemanticRefreshMode::AUTO;
}

// 'ADD' 'MATERIALIZATION' IfNotExists? ColId SemanticMaterializationOption* 'AS' SemanticQueryPiece*
unique_ptr<SQLStatement> TransformAddMaterialization(PEGTransformer &transformer, optional_ptr<ParseResult> action,
                                                     const QualifiedName &view, bool if_exists) {
	auto &list = List(action);
	ParsedSemanticMaterialization materialization;
	materialization.name = transformer.Transform<string>(list, 3);
	for (auto &option_node : Repeated(list.GetChild(4))) {
		auto option = Choice(option_node);
		if (option->name == "SemanticMaterializationWarehouse") {
			// 'WAREHOUSE' '=' ColId
			materialization.warehouse = transformer.Transform<string>(List(option), 2);
		} else if (option->name == "SemanticMaterializationStorage") {
			// 'IN' QualifiedName: <database> or <database>.<schema>
			auto storage = transformer.Transform<QualifiedName>(List(option), 1);
			if (!storage.catalog.empty() && storage.catalog != INVALID_CATALOG) {
				throw ParserException("ADD MATERIALIZATION ... IN expects <database> or <database>.<schema>");
			}
			bool has_schema = !storage.schema.empty() && storage.schema != INVALID_SCHEMA;
			materialization.storage_catalog = has_schema ? storage.schema : storage.name;
			materialization.storage_schema = has_schema ? storage.name : string();
		} else {
			// 'REFRESH_MODE' '=' ('AUTO' / 'FULL' / 'INCREMENTAL')
			materialization.refresh_mode = RefreshMode(Keyword(List(option).GetChild(2)));
		}
	}
	ParsedSemanticQuery body;
	body.view = view;
	TransformQueryPieces(transformer, list.GetChild(6), body);
	if (body.where) {
		throw ParserException("ADD MATERIALIZATION does not accept a WHERE clause");
	}
	for (auto &item : body.items) {
		if (item.kind == SemanticMemberKind::FACT) {
			throw ParserException("ADD MATERIALIZATION accepts DIMENSIONS and METRICS, not FACTS");
		}
	}
	materialization.items = std::move(body.items);
	return SemanticLowering::SelectAll(
	    SemanticLowering::AddMaterialization(view, if_exists, materialization, Present(list, 2)));
}

// 'ALTER' 'SEMANTIC' 'VIEW' IfExists? QualifiedName SemanticAlterAction
unique_ptr<SQLStatement> TransformAlterSemanticViewStmt(PEGTransformer &transformer,
                                                        optional_ptr<ParseResult> parse_result) {
	auto &list = List(parse_result);
	auto name = transformer.Transform<QualifiedName>(list, 4);
	auto if_exists = Present(list, 3);
	auto action = Choice(list.GetChild(5));
	if (action->name == "SemanticAlterRename") {
		// 'RENAME' 'TO' QualifiedName
		auto new_name = transformer.Transform<QualifiedName>(List(action), 2);
		if (!new_name.catalog.empty() || !new_name.schema.empty()) {
			throw ParserException("ALTER SEMANTIC VIEW ... RENAME TO only accepts an unqualified name");
		}
		return SemanticLowering::SelectAll(SemanticLowering::Rename(name, if_exists, new_name.name));
	}
	if (action->name == "SemanticAlterAddMaterialization") {
		return TransformAddMaterialization(transformer, action, name, if_exists);
	}
	if (action->name == "SemanticAlterDropMaterialization") {
		// 'DROP' 'MATERIALIZATION' IfExists? ColId
		auto &action_list = List(action);
		return SemanticLowering::SelectAll(SemanticLowering::DropMaterialization(
		    name, if_exists, transformer.Transform<string>(action_list, 3), Present(action_list, 2)));
	}
	if (action->name == "SemanticAlterRefreshMaterialization") {
		// 'REFRESH' 'MATERIALIZATION' ColId ('AUTO' / 'FULL' / 'INCREMENTAL')?
		auto &action_list = List(action);
		Value mode(LogicalType::VARCHAR);
		if (Present(action_list, 3)) {
			mode = Value(Keyword(Optional(action_list, 3)));
		}
		return SemanticLowering::SelectAll(SemanticLowering::RefreshMaterialization(
		    name, if_exists, transformer.Transform<string>(action_list, 2), mode));
	}
	if (action->name == "SemanticAlterSetMaxStaleness" || action->name == "SemanticAlterUnsetMaxStaleness") {
		// 'SET' 'MAX_STALENESS' '=' NumberLiteral / 'UNSET' 'MAX_STALENESS' (NULL)
		Value seconds(LogicalType::BIGINT);
		if (action->name == "SemanticAlterSetMaxStaleness") {
			auto value = transformer.Transform<unique_ptr<ParsedExpression>>(List(action), 3);
			seconds = Value::BIGINT(value->Cast<ConstantExpression>().value.GetValue<int64_t>());
		}
		return SemanticLowering::SelectAll(SemanticLowering::SetMaxStaleness(name, if_exists, seconds));
	}
	// 'SET' 'COMMENT' '=' SemanticString / 'UNSET' 'COMMENT' (a NULL comment)
	Value comment(LogicalType::VARCHAR);
	if (action->name == "SemanticAlterSetComment") {
		comment = Value(transformer.Transform<string>(List(action), 3));
	}
	return SemanticLowering::SelectAll(SemanticLowering::SetComment(name, if_exists, comment));
}

// ('DESCRIBE' / 'DESC') 'SEMANTIC' 'VIEW' QualifiedName
unique_ptr<TableRef> DescribeRef(PEGTransformer &transformer, optional_ptr<ParseResult> parse_result) {
	return SemanticLowering::Describe(transformer.Transform<QualifiedName>(List(parse_result), 3));
}

//! Applies the optional `'LIKE' StringLiteral` child at `index`
unique_ptr<TableRef> WithLike(PEGTransformer &transformer, unique_ptr<TableRef> ref, ListParseResult &list,
                              idx_t index) {
	if (!Present(list, index)) {
		return ref;
	}
	auto pattern = transformer.Transform<string>(List(Optional(list, index)), 1);
	return SemanticLowering::WhereNameLike(std::move(ref), pattern);
}

// 'SHOW' 'SEMANTIC' (SemanticShowViews / SemanticShowMembers)
unique_ptr<TableRef> ShowRef(PEGTransformer &transformer, optional_ptr<ParseResult> parse_result) {
	auto target = Choice(List(parse_result).GetChild(2));
	auto &list = List(target);
	if (target->name == "SemanticShowMaterializations") {
		// 'MATERIALIZATIONS' SemanticShowLike? ('IN' ('SEMANTIC' 'VIEW')? QualifiedName)?
		QualifiedName view;
		if (Present(list, 2)) {
			view = transformer.Transform<QualifiedName>(List(Optional(list, 2)), 2);
		}
		return WithLike(transformer, SemanticLowering::ShowMaterializations(view), list, 1);
	}
	if (target->name == "SemanticShowViews") {
		// 'VIEWS' SemanticShowLike? ('IN' ('SCHEMA' / 'DATABASE') QualifiedName)?
		QualifiedName scope;
		if (Present(list, 2)) {
			auto &scope_list = List(Optional(list, 2));
			auto scope_name = transformer.Transform<QualifiedName>(scope_list, 2);
			if (Keyword(scope_list.GetChild(1)) == "DATABASE") {
				scope.catalog = scope_name.name;
			} else {
				scope.catalog = scope_name.schema;
				scope.schema = scope_name.name;
			}
		}
		return WithLike(transformer, SemanticLowering::ShowViews(scope), list, 1);
	}
	// ('DIMENSIONS' / 'METRICS' / 'FACTS') SemanticShowLike? ('IN' ('SEMANTIC' 'VIEW')? QualifiedName)?
	// ('FOR' 'METRIC' SemanticDottedName)?
	auto kind = MemberKind(Keyword(list.GetChild(0)));
	QualifiedName view;
	if (Present(list, 2)) {
		view = transformer.Transform<QualifiedName>(List(Optional(list, 2)), 2);
	}
	if (!Present(list, 3)) {
		return WithLike(transformer, SemanticLowering::ShowMembers(kind, view), list, 1);
	}
	if (kind != SemanticMemberKind::DIMENSION) {
		throw ParserException("FOR METRIC is only supported with SHOW SEMANTIC DIMENSIONS");
	}
	if (!Present(list, 2)) {
		throw ParserException("SHOW SEMANTIC DIMENSIONS ... FOR METRIC requires IN <semantic view>");
	}
	auto metric = transformer.Transform<vector<string>>(List(Optional(list, 3)), 2);
	return WithLike(transformer, SemanticLowering::ShowDimensionsForMetric(view, metric), list, 1);
}

unique_ptr<SQLStatement> TransformDescribeSemanticViewStmt(PEGTransformer &transformer,
                                                           optional_ptr<ParseResult> parse_result) {
	return SemanticLowering::SelectAll(DescribeRef(transformer, parse_result));
}

unique_ptr<SQLStatement> TransformShowSemanticStmt(PEGTransformer &transformer,
                                                   optional_ptr<ParseResult> parse_result) {
	return SemanticLowering::SelectAll(ShowRef(transformer, parse_result));
}

unique_ptr<SQLStatement> TransformSemanticViewStatement(PEGTransformer &transformer,
                                                        optional_ptr<ParseResult> parse_result) {
	return transformer.Transform<unique_ptr<SQLStatement>>(Choice(parse_result));
}

// Parens(ShowSemanticStmt / DescribeSemanticViewStmt) TableAlias?
unique_ptr<TableRef> TransformSemanticShowTableRef(PEGTransformer &transformer,
                                                   optional_ptr<ParseResult> parse_result) {
	auto &list = List(parse_result);
	auto statement = Choice(PEGTransformerFactory::ExtractResultFromParens(list.GetChild(0)));
	auto result = statement->name == "ShowSemanticStmt" ? ShowRef(transformer, statement)
	                                                    : DescribeRef(transformer, statement);
	ApplyAlias(transformer, list, 1, *result);
	return result;
}

//===--------------------------------------------------------------------===//
// SEMANTIC_VIEW(...)
//===--------------------------------------------------------------------===//
//! Assembles the flat SEMANTIC_VIEW(...) body. The PEG matcher has no negative lookahead, so the clause keywords
//! (DIMENSIONS / METRICS / FACTS parse as bare column references), the items and their bare aliases are told apart
//! here: a clause keyword opens a clause, which takes one or more ','-separated items.
class SemanticQueryBodyBuilder {
public:
	explicit SemanticQueryBodyBuilder(ParsedSemanticQuery &query) : query(query) {
	}

	void Separator() {
		if (expecting_item) {
			throw ParserException("SEMANTIC_VIEW: unexpected ','");
		}
		expecting_item = true;
	}

	void AliasAs(const string &alias) {
		if (expecting_item || !CanAlias()) {
			throw ParserException("SEMANTIC_VIEW: unexpected AS");
		}
		query.items.back().alias = alias;
	}

	void Where(unique_ptr<ParsedExpression> where) {
		if (query.where) {
			throw ParserException("SEMANTIC_VIEW accepts at most one WHERE clause");
		}
		CloseClause();
		query.where = std::move(where);
		in_clause = false;
		expecting_item = true;
	}

	void Wildcard(const string &table) {
		if (!in_clause || !expecting_item) {
			throw ParserException("SEMANTIC_VIEW: expected DIMENSIONS, METRICS or FACTS before '%s.*'", table);
		}
		ParsedSemanticQueryItem item;
		item.star_table = table;
		AddItem(std::move(item));
	}

	void Expression(unique_ptr<ParsedExpression> expression) {
		auto bare_name = BareName(*expression);
		if (!bare_name.empty()) {
			auto upper = StringUtil::Upper(bare_name);
			if (upper == "DIMENSIONS" || upper == "METRICS" || upper == "FACTS") {
				OpenClause(MemberKind(upper));
				return;
			}
			if (!expecting_item) {
				// a bare name right after an item is that item's alias
				if (!CanAlias()) {
					throw ParserException("SEMANTIC_VIEW: unexpected '%s'", bare_name);
				}
				query.items.back().alias = bare_name;
				return;
			}
		}
		if (!in_clause) {
			throw ParserException("SEMANTIC_VIEW: expected DIMENSIONS, METRICS or FACTS before '%s'",
			                      expression->ToString());
		}
		if (!expecting_item) {
			throw ParserException("SEMANTIC_VIEW: expected ',' before '%s'", expression->ToString());
		}
		ParsedSemanticQueryItem item;
		item.expression = std::move(expression);
		AddItem(std::move(item));
	}

	void Finish() {
		CloseClause();
		if (query.items.empty()) {
			throw ParserException("SEMANTIC_VIEW requires at least one of DIMENSIONS, METRICS or FACTS");
		}
	}

private:
	static string BareName(const ParsedExpression &expression) {
		if (expression.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			return string();
		}
		auto &column = expression.Cast<ColumnRefExpression>();
		return column.IsQualified() ? string() : column.GetColumnName();
	}

	bool CanAlias() const {
		return !query.items.empty() && query.items.back().alias.empty() && query.items.back().expression;
	}

	void OpenClause(SemanticMemberKind new_kind) {
		CloseClause();
		kind = new_kind;
		in_clause = true;
		expecting_item = true;
		clause_items = 0;
	}

	//! A clause keyword must be followed by at least one item, and a ',' by another item
	void CloseClause() {
		if (in_clause && expecting_item) {
			throw ParserException("SEMANTIC_VIEW: %sS requires at least one item%s", SemanticMemberKindToString(kind),
			                      clause_items > 0 ? " after ','" : "");
		}
	}

	void AddItem(ParsedSemanticQueryItem item) {
		item.kind = kind;
		query.items.push_back(std::move(item));
		expecting_item = false;
		clause_items++;
	}

	ParsedSemanticQuery &query;
	SemanticMemberKind kind = SemanticMemberKind::DIMENSION;
	bool in_clause = false;
	bool expecting_item = true;
	idx_t clause_items = 0;
};

//! Fills `query` from a `SemanticQueryPiece*` child
void TransformQueryPieces(PEGTransformer &transformer, optional_ptr<ParseResult> pieces, ParsedSemanticQuery &query) {
	SemanticQueryBodyBuilder builder(query);
	for (auto &piece_node : Repeated(pieces)) {
		auto piece = Choice(piece_node);
		auto &piece_list = List(piece);
		if (piece->name == "SemanticQueryComma") {
			builder.Separator();
		} else if (piece->name == "SemanticQueryAliasAs") {
			// 'AS' ColLabel
			builder.AliasAs(transformer.Transform<string>(piece_list, 1));
		} else if (piece->name == "SemanticQueryStar") {
			// ColId '.' '*'
			builder.Wildcard(transformer.Transform<string>(piece_list, 0));
		} else if (piece->name == "SemanticQueryWhere") {
			// 'WHERE' Expression
			builder.Where(transformer.Transform<unique_ptr<ParsedExpression>>(piece_list, 1));
		} else {
			builder.Expression(transformer.Transform<unique_ptr<ParsedExpression>>(piece_list, 0));
		}
	}
	builder.Finish();
}

// QualifiedName SemanticQueryPiece*
unique_ptr<ParsedSemanticQuery> TransformSemanticViewQueryBody(PEGTransformer &transformer,
                                                               optional_ptr<ParseResult> parse_result) {
	auto &list = List(parse_result);
	auto query = make_uniq<ParsedSemanticQuery>();
	query->view = transformer.Transform<QualifiedName>(list, 0);
	TransformQueryPieces(transformer, list.GetChild(1), *query);
	return query;
}

// ('SEMANTIC_VIEW' / 'SEMANTIC' 'VIEW') Parens(SemanticViewQueryBody) TableAlias?
unique_ptr<TableRef> TransformSemanticViewRef(PEGTransformer &transformer, optional_ptr<ParseResult> parse_result) {
	auto &list = List(parse_result);
	auto body = PEGTransformerFactory::ExtractResultFromParens(list.GetChild(1));
	auto result = SemanticLowering::Query(*transformer.Transform<unique_ptr<ParsedSemanticQuery>>(body));
	ApplyAlias(transformer, list, 2, *result);
	return result;
}

} // namespace

void PEGTransformerFactory::RegisterSemanticView() {
	// statements and table references
	Register("SemanticViewStatement", &TransformSemanticViewStatement);
	Register("CreateSemanticViewStmt", &TransformCreateSemanticViewStmt);
	Register("DropSemanticViewStmt", &TransformDropSemanticViewStmt);
	Register("AlterSemanticViewStmt", &TransformAlterSemanticViewStmt);
	Register("DescribeSemanticViewStmt", &TransformDescribeSemanticViewStmt);
	Register("ShowSemanticStmt", &TransformShowSemanticStmt);
	Register("SemanticShowTableRef", &TransformSemanticShowTableRef);
	Register("SemanticViewRef", &TransformSemanticViewRef);
	Register("SemanticViewQueryBody", &TransformSemanticViewQueryBody);
	// definition
	Register("SemanticViewDefinitionBody", &TransformSemanticViewDefinitionBody);
	Register("SemanticTablesClause", &TransformSemanticTablesClause);
	Register("SemanticLogicalTable", &TransformSemanticLogicalTable);
	Register("SemanticTableAliasAs", &TransformSemanticName);
	Register("SemanticTableSource", &TransformSemanticTableSource);
	Register("SemanticRangeConstraint", &TransformSemanticRangeConstraint);
	Register("SemanticConstraintName", &TransformSemanticName);
	Register("SemanticRelationshipsClause", &TransformSemanticRelationshipsClause);
	Register("SemanticRelationship", &TransformSemanticRelationship);
	Register("SemanticRelationshipName", &TransformSemanticName);
	Register("SemanticFactsClause", &TransformSemanticMemberClause);
	Register("SemanticDimensionsClause", &TransformSemanticMemberClause);
	Register("SemanticMetricsClause", &TransformSemanticMemberClause);
	Register("SemanticMember", &TransformSemanticMember);
	Register("SemanticMemberName", &TransformSemanticMemberName);
	Register("SemanticVerifiedQuery", &TransformSemanticVerifiedQuery);
	// shared clauses
	Register("SemanticColumnList", &TransformSemanticColumnList);
	Register("SemanticWithSynonyms", &TransformSemanticWithSynonyms);
	Register("SemanticComment", &TransformSemanticComment);
	Register("SemanticString", &TransformSemanticString);
	Register("SemanticTag", &TransformSemanticTag);
	Register("SemanticTagEntry", &TransformSemanticTagEntry);
	Register("SemanticDottedName", &TransformSemanticDottedName);
}

} // namespace semantic_views_peg
} // namespace duckdb
