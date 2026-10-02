#include "transformer/peg_transformer.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "parsed_semantic_view.hpp"
#include "semantic_view_peg_context.hpp"

namespace duckdb {

thread_local unique_ptr<ParsedSemanticView> *SemanticViewPegContext::definition_capture = nullptr;

namespace semantic_views_peg {

namespace {

//===--------------------------------------------------------------------===//
// Parse-tree helpers
//===--------------------------------------------------------------------===//
bool IsWordLike(const string &text) {
	if (text.empty()) {
		return false;
	}
	for (auto c : text) {
		if (!StringUtil::CharacterIsAlphaNumeric(c) && c != '_' && c != '$' && static_cast<unsigned char>(c) < 128) {
			return false;
		}
	}
	return true;
}

//! Nodes whose subtrees belong to DuckDB's own grammar - never searched for semantic-view rules
bool IsOpaque(const ParseResult &pr) {
	return pr.name == "Expression" || pr.name == "SelectStatementInternal" || pr.name == "OrderByExpression" ||
	       pr.name == "QualifiedName" || pr.name == "BaseTableName" || pr.name == "TableAlias";
}

void Children(optional_ptr<ParseResult> pr, vector<optional_ptr<ParseResult>> &out) {
	if (!pr) {
		return;
	}
	switch (pr->type) {
	case ParseResultType::LIST:
		for (auto &child : pr->Cast<ListParseResult>().GetChildren()) {
			if (child) {
				out.push_back(child);
			}
		}
		break;
	case ParseResultType::REPEAT:
		for (auto &child : pr->Cast<RepeatParseResult>().children) {
			if (child) {
				out.push_back(child);
			}
		}
		break;
	case ParseResultType::OPTIONAL: {
		auto &opt = pr->Cast<OptionalParseResult>();
		if (opt.HasResult()) {
			out.push_back(opt.optional_result);
		}
		break;
	}
	case ParseResultType::CHOICE: {
		auto &choice = pr->Cast<ChoiceParseResult>();
		if (choice.result) {
			out.push_back(choice.result);
		}
		break;
	}
	default:
		break;
	}
}

//! Collects the descendants (not `pr` itself) named `name`, without descending into matches or opaque nodes
void CollectNamed(optional_ptr<ParseResult> pr, const string &name, vector<optional_ptr<ParseResult>> &out) {
	vector<optional_ptr<ParseResult>> children;
	Children(pr, children);
	for (auto &child : children) {
		if (StringUtil::CIEquals(child->name, name)) {
			// Choice / optional wrappers copy the name of the rule they wrap: return the rule's own node
			auto node = child;
			while (node && (node->type == ParseResultType::CHOICE || node->type == ParseResultType::OPTIONAL)) {
				vector<optional_ptr<ParseResult>> inner;
				Children(node, inner);
				if (inner.empty() || !StringUtil::CIEquals(inner[0]->name, name)) {
					break;
				}
				node = inner[0];
			}
			out.push_back(node);
			continue;
		}
		if (IsOpaque(*child)) {
			continue;
		}
		CollectNamed(child, name, out);
	}
}

vector<optional_ptr<ParseResult>> AllNamed(optional_ptr<ParseResult> pr, const string &name) {
	vector<optional_ptr<ParseResult>> result;
	CollectNamed(pr, name, result);
	return result;
}

optional_ptr<ParseResult> FirstNamed(optional_ptr<ParseResult> pr, const string &name) {
	auto all = AllNamed(pr, name);
	return all.empty() ? nullptr : all[0];
}

//! Word-like leaves (identifiers, keywords, string and number literals) in order
void CollectLeaves(optional_ptr<ParseResult> pr, vector<string> &out) {
	if (!pr) {
		return;
	}
	switch (pr->type) {
	case ParseResultType::IDENTIFIER:
		out.push_back(pr->Cast<IdentifierParseResult>().identifier);
		return;
	case ParseResultType::KEYWORD: {
		auto &keyword = pr->Cast<KeywordParseResult>().keyword;
		if (IsWordLike(keyword)) {
			out.push_back(keyword);
		}
		return;
	}
	case ParseResultType::STRING:
		out.push_back(pr->Cast<StringLiteralParseResult>().result);
		return;
	case ParseResultType::NUMBER:
		out.push_back(pr->Cast<NumberParseResult>().number);
		return;
	default:
		break;
	}
	vector<optional_ptr<ParseResult>> children;
	Children(pr, children);
	for (auto &child : children) {
		CollectLeaves(child, out);
	}
}

string LeafText(optional_ptr<ParseResult> pr) {
	vector<string> leaves;
	CollectLeaves(pr, leaves);
	if (leaves.empty()) {
		throw InternalException("semantic view parser: expected an identifier or literal in rule '%s'",
		                        pr ? pr->name : string("<null>"));
	}
	return leaves[0];
}

vector<string> NamedLeafTexts(optional_ptr<ParseResult> pr, const string &name) {
	vector<string> result;
	for (auto &node : AllNamed(pr, name)) {
		result.push_back(LeafText(node));
	}
	return result;
}

string StringLiteralValue(optional_ptr<ParseResult> pr) {
	auto literal = FirstNamed(pr, "StringLiteral");
	if (!literal) {
		throw InternalException("semantic view parser: expected a string literal in rule '%s'", pr->name);
	}
	return LeafText(literal);
}

vector<string> StringLiteralList(optional_ptr<ParseResult> pr) {
	return NamedLeafTexts(pr, "StringLiteral");
}

vector<pair<string, string>> ParseTags(optional_ptr<ParseResult> tag) {
	vector<pair<string, string>> result;
	for (auto &entry : AllNamed(tag, "SemanticTagEntry")) {
		auto dotted = FirstNamed(entry, "SemanticDottedName");
		vector<string> parts;
		CollectLeaves(dotted, parts);
		result.emplace_back(StringUtil::Join(parts, "."), StringLiteralValue(entry));
	}
	return result;
}

string DottedName(optional_ptr<ParseResult> pr) {
	vector<string> parts;
	CollectLeaves(pr, parts);
	return StringUtil::Join(parts, ".");
}

//===--------------------------------------------------------------------===//
// Lowering helpers
//===--------------------------------------------------------------------===//
unique_ptr<SQLStatement> MakeFunctionSelect(const string &function_name, vector<Value> args) {
	vector<unique_ptr<ParsedExpression>> children;
	for (auto &arg : args) {
		children.push_back(make_uniq<ConstantExpression>(std::move(arg)));
	}
	auto ref = make_uniq<TableFunctionRef>();
	ref->function = make_uniq<FunctionExpression>(function_name, std::move(children));
	auto node = make_uniq<SelectNode>();
	node->select_list.push_back(make_uniq<StarExpression>());
	node->from_table = std::move(ref);
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(node);
	return std::move(statement);
}

Value NameValue(const string &part) {
	return Value(part);
}

//===--------------------------------------------------------------------===//
// CREATE SEMANTIC VIEW
//===--------------------------------------------------------------------===//
ParsedSemanticTable ParseLogicalTable(PEGTransformer &transformer, optional_ptr<ParseResult> pr) {
	ParsedSemanticTable table;
	auto alias = FirstNamed(pr, "SemanticTableAliasAs");
	if (alias) {
		table.alias = LeafText(alias);
	}
	auto source = FirstNamed(pr, "SemanticTableSource");
	auto select = FirstNamed(source, "SelectStatementInternal");
	if (select) {
		auto subquery = transformer.Transform<unique_ptr<SelectStatement>>(select);
		table.source = make_uniq<SubqueryRef>(std::move(subquery));
	} else {
		auto base = FirstNamed(source, "BaseTableName");
		if (!base) {
			throw ParserException("semantic view: expected a table name or a parenthesized query in TABLES");
		}
		auto base_ref = transformer.Transform<unique_ptr<BaseTableRef>>(base);
		table.source = std::move(base_ref);
	}
	for (auto &option : AllNamed(pr, "SemanticTableOption")) {
		auto key = FirstNamed(option, "SemanticPrimaryKey");
		if (key) {
			if (!table.primary_key.empty()) {
				throw ParserException("semantic view: logical table has more than one PRIMARY KEY");
			}
			table.primary_key = NamedLeafTexts(key, "ColId");
			continue;
		}
		auto unique = FirstNamed(option, "SemanticUnique");
		if (unique) {
			table.unique_keys.push_back(NamedLeafTexts(unique, "ColId"));
			continue;
		}
		auto range = FirstNamed(option, "SemanticRangeConstraint");
		if (range) {
			ParsedSemanticRangeConstraint constraint;
			auto name = FirstNamed(range, "SemanticConstraintName");
			if (name) {
				constraint.name = LeafText(name);
			}
			// the last two ColIds are the range bounds (an optional constraint name precedes them)
			auto columns = NamedLeafTexts(range, "ColId");
			if (columns.size() < 2) {
				throw ParserException("semantic view: invalid DISTINCT RANGE constraint");
			}
			constraint.start_column = columns[columns.size() - 2];
			constraint.end_column = columns[columns.size() - 1];
			table.range_constraints.push_back(std::move(constraint));
			continue;
		}
		auto synonyms = FirstNamed(option, "SemanticWithSynonyms");
		if (synonyms) {
			table.synonyms = StringLiteralList(synonyms);
			continue;
		}
		auto comment = FirstNamed(option, "SemanticComment");
		if (comment) {
			table.comment = StringLiteralValue(comment);
			continue;
		}
		auto tag = FirstNamed(option, "SemanticTag");
		if (tag) {
			table.tags = ParseTags(tag);
			continue;
		}
	}
	return table;
}

ParsedSemanticRelationship ParseRelationship(optional_ptr<ParseResult> pr) {
	// SemanticRelationship <- SemanticRelationshipName? ColId SemanticColumnList 'REFERENCES' ColId
	//                         SemanticReferencedColumns?
	auto &list_pr = pr->Cast<ListParseResult>();
	ParsedSemanticRelationship rel;
	auto &name = list_pr.Child<OptionalParseResult>(0);
	if (name.HasResult()) {
		rel.name = LeafText(name.optional_result);
	}
	rel.from_table = LeafText(list_pr.GetChild(1));
	rel.from_columns = NamedLeafTexts(list_pr.GetChild(2), "ColId");
	rel.to_table = LeafText(list_pr.GetChild(4));
	auto &referenced = list_pr.Child<OptionalParseResult>(5);
	if (referenced.HasResult()) {
		auto range = FirstNamed(referenced.optional_result, "SemanticRangeReference");
		if (range) {
			auto bounds = NamedLeafTexts(range, "ColId");
			rel.is_range = true;
			rel.range_start = bounds[0];
			rel.range_end = bounds[1];
		} else {
			auto columns = AllNamed(referenced.optional_result, "SemanticReferencedColumn");
			for (idx_t i = 0; i < columns.size(); i++) {
				if (FirstNamed(columns[i], "SemanticAsof")) {
					if (rel.asof_index != DConstants::INVALID_INDEX) {
						throw ParserException("semantic view: at most one ASOF column per relationship");
					}
					rel.asof_index = i;
				}
				rel.to_columns.push_back(LeafText(FirstNamed(columns[i], "ColId")));
			}
		}
	}
	return rel;
}

ParsedSemanticMember ParseMember(PEGTransformer &transformer, optional_ptr<ParseResult> pr, SemanticMemberKind kind) {
	ParsedSemanticMember member;
	member.kind = kind;
	auto visibility = FirstNamed(pr, "SemanticVisibility");
	if (visibility) {
		member.is_private = StringUtil::CIEquals(LeafText(visibility), "PRIVATE");
		if (member.is_private && kind == SemanticMemberKind::DIMENSION) {
			throw ParserException("semantic view: dimensions cannot be PRIVATE");
		}
	}
	auto name = FirstNamed(pr, "SemanticMemberName");
	vector<string> name_parts;
	CollectLeaves(name, name_parts);
	if (name_parts.size() == 2) {
		member.table = name_parts[0];
		member.name = name_parts[1];
	} else if (name_parts.size() == 1) {
		if (kind != SemanticMemberKind::METRIC) {
			throw ParserException("semantic view: %s '%s' must be qualified with its logical table (table.name)",
			                      StringUtil::Lower(SemanticMemberKindToString(kind)), name_parts[0]);
		}
		member.name = name_parts[0];
	} else {
		throw ParserException("semantic view: invalid member name");
	}
	auto labels = FirstNamed(pr, "SemanticLabels");
	if (labels) {
		member.labels = NamedLeafTexts(labels, "ColId");
	}
	auto using_clause = FirstNamed(pr, "SemanticUsing");
	if (using_clause) {
		member.using_relationships = NamedLeafTexts(using_clause, "ColId");
	}
	auto non_additive = FirstNamed(pr, "SemanticNonAdditive");
	if (non_additive) {
		for (auto &order : AllNamed(non_additive, "OrderByExpression")) {
			member.non_additive_by.push_back(transformer.Transform<OrderByNode>(order));
		}
	}
	auto expression = FirstNamed(pr, "Expression");
	if (!expression) {
		throw ParserException("semantic view: missing AS <expression> for '%s'", member.FullName());
	}
	member.expression = transformer.Transform<unique_ptr<ParsedExpression>>(expression);
	for (auto &option : AllNamed(pr, "SemanticMemberOption")) {
		if (auto synonyms = FirstNamed(option, "SemanticWithSynonyms")) {
			member.synonyms = StringLiteralList(synonyms);
		} else if (auto comment = FirstNamed(option, "SemanticComment")) {
			member.comment = StringLiteralValue(comment);
		} else if (auto tag = FirstNamed(option, "SemanticTag")) {
			member.tags = ParseTags(tag);
		} else if (auto samples = FirstNamed(option, "SemanticSampleValues")) {
			member.sample_values = StringLiteralList(samples);
		} else if (FirstNamed(option, "SemanticIsEnum")) {
			member.is_enum = true;
		} else if (auto cortex = FirstNamed(option, "SemanticCortexSearch")) {
			member.cortex_search_service = DottedName(FirstNamed(cortex, "SemanticDottedName"));
			auto column = FirstNamed(cortex, "SemanticCortexSearchColumn");
			if (column) {
				member.cortex_search_column = LeafText(FirstNamed(column, "ColId"));
			}
		}
	}
	return member;
}

void ParseMembers(PEGTransformer &transformer, optional_ptr<ParseResult> clause, SemanticMemberKind kind,
                  vector<ParsedSemanticMember> &out) {
	if (!clause) {
		return;
	}
	for (auto &member : AllNamed(clause, "SemanticMember")) {
		out.push_back(ParseMember(transformer, member, kind));
	}
}

void ParseViewProperties(optional_ptr<ParseResult> body, ParsedSemanticView &view) {
	for (auto &property : AllNamed(body, "SemanticViewProperty")) {
		if (auto comment = FirstNamed(property, "SemanticComment")) {
			view.comment = StringLiteralValue(comment);
		} else if (auto staleness = FirstNamed(property, "SemanticMaxStaleness")) {
			vector<string> leaves;
			CollectLeaves(staleness, leaves);
			view.has_max_staleness = true;
			view.max_staleness = std::stoll(leaves.back());
		} else if (auto ai_sql = FirstNamed(property, "SemanticAiSqlGeneration")) {
			view.ai_sql_generation = StringLiteralValue(ai_sql);
		} else if (auto ai_cat = FirstNamed(property, "SemanticAiQuestionCategorization")) {
			view.ai_question_categorization = StringLiteralValue(ai_cat);
		} else if (auto verified = FirstNamed(property, "SemanticAiVerifiedQueries")) {
			for (auto &query : AllNamed(verified, "SemanticVerifiedQuery")) {
				ParsedSemanticVerifiedQuery vq;
				vq.name = LeafText(FirstNamed(query, "ColId"));
				for (auto &vq_property : AllNamed(query, "SemanticVerifiedQueryProperty")) {
					vector<string> leaves;
					CollectLeaves(vq_property, leaves);
					if (leaves.size() < 2) {
						continue;
					}
					auto keyword = StringUtil::Upper(leaves[0]);
					auto value = leaves[1];
					if (FirstNamed(vq_property, "StringLiteral")) {
						value = SemanticQuoteString(value);
					}
					vq.properties.emplace_back(keyword, value);
				}
				view.verified_queries.push_back(std::move(vq));
			}
		} else if (auto tag = FirstNamed(property, "SemanticTag")) {
			view.tags = ParseTags(tag);
		} else if (FirstNamed(property, "SemanticCopyGrants")) {
			view.copy_grants = true;
		}
	}
}

} // namespace

//===--------------------------------------------------------------------===//
// Statement transforms
//===--------------------------------------------------------------------===//
static unique_ptr<SQLStatement> TransformCreateSemanticViewStmt(PEGTransformer &transformer,
                                                                optional_ptr<ParseResult> parse_result) {
	auto view = make_uniq<ParsedSemanticView>();
	string conflict = "error";
	auto modifier = FirstNamed(parse_result, "SemanticReplaceOrAlter");
	if (modifier) {
		conflict = "replace";
	}
	if (FirstNamed(parse_result, "IfNotExists")) {
		if (modifier) {
			throw ParserException("CREATE OR REPLACE SEMANTIC VIEW cannot be combined with IF NOT EXISTS");
		}
		conflict = "ignore";
	}
	view->name = transformer.Transform<QualifiedName>(FirstNamed(parse_result, "QualifiedName"));
	auto body = FirstNamed(parse_result, "SemanticViewDefinitionBody");
	for (auto &table : AllNamed(FirstNamed(body, "SemanticTablesClause"), "SemanticLogicalTable")) {
		view->tables.push_back(ParseLogicalTable(transformer, table));
	}
	auto relationships = FirstNamed(body, "SemanticRelationshipsClause");
	if (relationships) {
		for (auto &rel : AllNamed(relationships, "SemanticRelationship")) {
			view->relationships.push_back(ParseRelationship(rel));
		}
	}
	ParseMembers(transformer, FirstNamed(body, "SemanticFactsClause"), SemanticMemberKind::FACT, view->facts);
	ParseMembers(transformer, FirstNamed(body, "SemanticDimensionsClause"), SemanticMemberKind::DIMENSION,
	             view->dimensions);
	ParseMembers(transformer, FirstNamed(body, "SemanticMetricsClause"), SemanticMemberKind::METRIC, view->metrics);
	ParseViewProperties(body, *view);

	auto canonical_sql = view->ToSQL();
	auto name = view->name;
	if (SemanticViewPegContext::definition_capture) {
		*SemanticViewPegContext::definition_capture = std::move(view);
	}
	return MakeFunctionSelect(SEMANTIC_VIEW_DDL_FUNCTION,
	                          {Value("create"), NameValue(name.catalog), NameValue(name.schema), NameValue(name.name),
	                           Value(canonical_sql), Value(conflict)});
}

static unique_ptr<SQLStatement> TransformDropSemanticViewStmt(PEGTransformer &transformer,
                                                              optional_ptr<ParseResult> parse_result) {
	auto name = transformer.Transform<QualifiedName>(FirstNamed(parse_result, "QualifiedName"));
	auto flag = FirstNamed(parse_result, "IfExists") ? "if_exists" : "error";
	return MakeFunctionSelect(SEMANTIC_VIEW_DDL_FUNCTION, {Value("drop"), NameValue(name.catalog),
	                                                       NameValue(name.schema), NameValue(name.name), Value(),
	                                                       Value(flag)});
}

static unique_ptr<SQLStatement> TransformAlterSemanticViewStmt(PEGTransformer &transformer,
                                                               optional_ptr<ParseResult> parse_result) {
	auto names = AllNamed(parse_result, "QualifiedName");
	auto name = transformer.Transform<QualifiedName>(names[0]);
	auto flag = FirstNamed(parse_result, "IfExists") ? "if_exists" : "error";
	string action;
	Value arg;
	if (FirstNamed(parse_result, "SemanticAlterRename")) {
		action = "rename";
		auto new_name = transformer.Transform<QualifiedName>(names[1]);
		if ((!new_name.catalog.empty() && new_name.catalog != INVALID_CATALOG) ||
		    (!new_name.schema.empty() && new_name.schema != INVALID_SCHEMA)) {
			throw ParserException("ALTER SEMANTIC VIEW ... RENAME TO only accepts an unqualified name");
		}
		arg = Value(new_name.name);
	} else if (auto set_comment = FirstNamed(parse_result, "SemanticAlterSetComment")) {
		action = "set_comment";
		arg = Value(StringLiteralValue(set_comment));
	} else {
		action = "unset_comment";
	}
	return MakeFunctionSelect(SEMANTIC_VIEW_DDL_FUNCTION,
	                          {Value(action), NameValue(name.catalog), NameValue(name.schema), NameValue(name.name),
	                           std::move(arg), Value(flag)});
}

static unique_ptr<SQLStatement> TransformDescribeSemanticViewStmt(PEGTransformer &transformer,
                                                                  optional_ptr<ParseResult> parse_result) {
	auto name = transformer.Transform<QualifiedName>(FirstNamed(parse_result, "QualifiedName"));
	return MakeFunctionSelect(SEMANTIC_VIEW_SHOW_FUNCTION, {Value("describe"), NameValue(name.catalog),
	                                                        NameValue(name.schema), NameValue(name.name), Value(),
	                                                        Value()});
}

static unique_ptr<SQLStatement> TransformShowSemanticStmt(PEGTransformer &transformer,
                                                          optional_ptr<ParseResult> parse_result) {
	Value like;
	auto like_node = FirstNamed(parse_result, "SemanticShowLike");
	if (like_node) {
		like = Value(StringLiteralValue(like_node));
	}
	if (auto views = FirstNamed(parse_result, "SemanticShowViews")) {
		string catalog, schema, scope;
		auto scope_node = FirstNamed(views, "SemanticShowViewsScope");
		if (scope_node) {
			auto scope_name = transformer.Transform<QualifiedName>(FirstNamed(scope_node, "QualifiedName"));
			if (StringUtil::CIEquals(LeafText(FirstNamed(scope_node, "SemanticShowScopeKind")), "DATABASE")) {
				scope = "database";
				catalog = scope_name.name;
			} else {
				scope = "schema";
				catalog = scope_name.schema == INVALID_SCHEMA ? string() : scope_name.schema;
				schema = scope_name.name;
			}
		}
		return MakeFunctionSelect(SEMANTIC_VIEW_SHOW_FUNCTION,
		                          {Value("views"), Value(catalog), Value(schema), Value(), like, Value(scope)});
	}
	auto members = FirstNamed(parse_result, "SemanticShowMembers");
	auto kind = StringUtil::Lower(LeafText(FirstNamed(members, "SemanticShowMemberKind")));
	QualifiedName name;
	name.catalog = INVALID_CATALOG;
	name.schema = INVALID_SCHEMA;
	auto in_node = FirstNamed(members, "SemanticShowIn");
	if (in_node) {
		name = transformer.Transform<QualifiedName>(FirstNamed(in_node, "QualifiedName"));
	}
	Value for_metric;
	auto for_node = FirstNamed(members, "SemanticShowForMetric");
	if (for_node) {
		if (kind != "dimensions") {
			throw ParserException("FOR METRIC is only supported with SHOW SEMANTIC DIMENSIONS");
		}
		if (!in_node) {
			throw ParserException("SHOW SEMANTIC DIMENSIONS ... FOR METRIC requires IN <semantic view>");
		}
		for_metric = Value(DottedName(FirstNamed(for_node, "SemanticDottedName")));
	}
	return MakeFunctionSelect(SEMANTIC_VIEW_SHOW_FUNCTION, {Value(kind), NameValue(name.catalog),
	                                                        NameValue(name.schema), NameValue(name.name), like,
	                                                        for_metric});
}

static unique_ptr<SQLStatement> TransformSemanticViewStatement(PEGTransformer &transformer,
                                                               optional_ptr<ParseResult> parse_result) {
	vector<optional_ptr<ParseResult>> children;
	Children(parse_result, children);
	while (children.size() == 1 && children[0]->name == "SemanticViewStatement") {
		auto node = children[0];
		children.clear();
		Children(node, children);
	}
	for (auto &child : children) {
		if (child->name == "CreateSemanticViewStmt") {
			return TransformCreateSemanticViewStmt(transformer, child);
		}
		if (child->name == "DropSemanticViewStmt") {
			return TransformDropSemanticViewStmt(transformer, child);
		}
		if (child->name == "AlterSemanticViewStmt") {
			return TransformAlterSemanticViewStmt(transformer, child);
		}
		if (child->name == "DescribeSemanticViewStmt") {
			return TransformDescribeSemanticViewStmt(transformer, child);
		}
		if (child->name == "ShowSemanticStmt") {
			return TransformShowSemanticStmt(transformer, child);
		}
	}
	throw InternalException("semantic view parser: unrecognized statement");
}

//===--------------------------------------------------------------------===//
// SEMANTIC_VIEW(...) table reference
//===--------------------------------------------------------------------===//
static unique_ptr<TableRef> TransformSemanticViewRef(PEGTransformer &transformer,
                                                     optional_ptr<ParseResult> parse_result) {
	ParsedSemanticQuery query;
	auto body = FirstNamed(parse_result, "SemanticViewQueryBody");
	query.view = transformer.Transform<QualifiedName>(FirstNamed(body, "QualifiedName"));
	bool has_kind = false;
	bool expecting_item = true;
	SemanticMemberKind kind = SemanticMemberKind::DIMENSION;
	for (auto &piece : AllNamed(body, "SemanticQueryPiece")) {
		if (FirstNamed(piece, "SemanticQueryComma")) {
			if (expecting_item) {
				throw ParserException("SEMANTIC_VIEW: unexpected ','");
			}
			expecting_item = true;
			continue;
		}
		if (auto alias = FirstNamed(piece, "SemanticQueryAliasAs")) {
			if (expecting_item || query.items.empty() || !query.items.back().alias.empty() ||
			    !query.items.back().expression) {
				throw ParserException("SEMANTIC_VIEW: unexpected AS");
			}
			query.items.back().alias = LeafText(FirstNamed(alias, "ColLabel"));
			continue;
		}
		if (auto where = FirstNamed(piece, "SemanticQueryWhere")) {
			if (query.where) {
				throw ParserException("SEMANTIC_VIEW accepts at most one WHERE clause");
			}
			query.where = transformer.Transform<unique_ptr<ParsedExpression>>(FirstNamed(where, "Expression"));
			has_kind = false;
			expecting_item = true;
			continue;
		}
		if (auto star = FirstNamed(piece, "SemanticQueryStar")) {
			if (!has_kind || !expecting_item) {
				throw ParserException("SEMANTIC_VIEW: expected DIMENSIONS, METRICS or FACTS before '%s.*'",
				                      LeafText(FirstNamed(star, "ColId")));
			}
			ParsedSemanticQueryItem item;
			item.kind = kind;
			item.star_table = LeafText(FirstNamed(star, "ColId"));
			query.items.push_back(std::move(item));
			expecting_item = false;
			continue;
		}
		auto expression = transformer.Transform<unique_ptr<ParsedExpression>>(FirstNamed(piece, "Expression"));
		string bare_name;
		if (expression->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &colref = expression->Cast<ColumnRefExpression>();
			if (!colref.IsQualified()) {
				bare_name = colref.GetColumnName();
			}
		}
		if (!bare_name.empty()) {
			// DIMENSIONS / METRICS / FACTS parse as bare column references: they open a clause
			auto upper = StringUtil::Upper(bare_name);
			if (upper == "DIMENSIONS" || upper == "METRICS" || upper == "FACTS") {
				kind = upper == "DIMENSIONS" ? SemanticMemberKind::DIMENSION
				       : upper == "METRICS"  ? SemanticMemberKind::METRIC
				                             : SemanticMemberKind::FACT;
				has_kind = true;
				expecting_item = true;
				continue;
			}
			if (!expecting_item) {
				// a bare name right after an item is that item's alias
				auto &last = query.items.back();
				if (!last.alias.empty() || !last.expression) {
					throw ParserException("SEMANTIC_VIEW: unexpected '%s'", bare_name);
				}
				last.alias = bare_name;
				continue;
			}
		}
		if (!has_kind) {
			throw ParserException("SEMANTIC_VIEW: expected DIMENSIONS, METRICS or FACTS before '%s'",
			                      expression->ToString());
		}
		if (!expecting_item) {
			throw ParserException("SEMANTIC_VIEW: expected ',' before '%s'", expression->ToString());
		}
		ParsedSemanticQueryItem item;
		item.kind = kind;
		item.expression = std::move(expression);
		query.items.push_back(std::move(item));
		expecting_item = false;
	}
	if (query.items.empty()) {
		throw ParserException("SEMANTIC_VIEW requires at least one of DIMENSIONS, METRICS or FACTS");
	}

	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(NameValue(query.view.catalog)));
	children.push_back(make_uniq<ConstantExpression>(NameValue(query.view.schema)));
	children.push_back(make_uniq<ConstantExpression>(NameValue(query.view.name)));
	children.push_back(make_uniq<ConstantExpression>(query.ItemsToValue()));
	children.push_back(
	    make_uniq<ConstantExpression>(Value(query.where ? query.where->ToString() : string())));
	auto result = make_uniq<TableFunctionRef>();
	result->function = make_uniq<FunctionExpression>(SEMANTIC_VIEW_QUERY_FUNCTION, std::move(children));

	auto alias_node = FirstNamed(parse_result, "TableAlias");
	if (alias_node) {
		auto table_alias = transformer.Transform<TableAlias>(alias_node);
		result->alias = table_alias.name;
		result->column_name_alias = table_alias.column_name_alias;
	}
	return std::move(result);
}

static unique_ptr<TableRef> TransformSemanticShowTableRef(PEGTransformer &transformer,
                                                          optional_ptr<ParseResult> parse_result) {
	unique_ptr<SQLStatement> statement;
	if (auto show = FirstNamed(parse_result, "ShowSemanticStmt")) {
		statement = TransformShowSemanticStmt(transformer, show);
	} else {
		statement = TransformDescribeSemanticViewStmt(transformer, FirstNamed(parse_result, "DescribeSemanticViewStmt"));
	}
	auto &select = statement->Cast<SelectStatement>();
	auto result = std::move(select.node->Cast<SelectNode>().from_table);
	auto alias_node = FirstNamed(parse_result, "TableAlias");
	if (alias_node) {
		auto table_alias = transformer.Transform<TableAlias>(alias_node);
		result->alias = table_alias.name;
		result->column_name_alias = table_alias.column_name_alias;
	}
	return result;
}

void PEGTransformerFactory::RegisterSemanticView() {
	Register("SemanticViewStatement", &TransformSemanticViewStatement);
	Register("SemanticShowTableRef", &TransformSemanticShowTableRef);
	Register("SemanticViewRef", &TransformSemanticViewRef);
}

} // namespace semantic_views_peg
} // namespace duckdb
