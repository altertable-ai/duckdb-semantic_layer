#include "semantic_views_grammar.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "parsed_semantic_view.hpp"
#include "semantic_lowering.hpp"
#include "semantic_markers.hpp"

namespace duckdb {

DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("semantic_views::ParsedSemanticTag", ParsedSemanticTag)
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("semantic_views::vector<ParsedSemanticTag>", vector<ParsedSemanticTag>)
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("semantic_views::ParsedSemanticTable", ParsedSemanticTable)
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("semantic_views::vector<ParsedSemanticTable>", vector<ParsedSemanticTable>)
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("semantic_views::ParsedSemanticRangeConstraint", ParsedSemanticRangeConstraint)
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("semantic_views::ParsedSemanticRelationship", ParsedSemanticRelationship)
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("semantic_views::vector<ParsedSemanticRelationship>",
                                      vector<ParsedSemanticRelationship>)
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("semantic_views::ParsedSemanticMember", ParsedSemanticMember)
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("semantic_views::vector<ParsedSemanticMember>", vector<ParsedSemanticMember>)
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("semantic_views::ParsedSemanticVerifiedQuery", ParsedSemanticVerifiedQuery)
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("semantic_views::unique_ptr<ParsedSemanticView>", unique_ptr<ParsedSemanticView>)
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("semantic_views::unique_ptr<ParsedSemanticQuery>",
                                      unique_ptr<ParsedSemanticQuery>)

namespace {

//===--------------------------------------------------------------------===//
// Grammar
//===--------------------------------------------------------------------===//
// One PEG rule per entry; they reuse DuckDB's own rules (Expression,
// QualifiedName, ColId, ...). Clause keywords (TABLES, DIMENSIONS, METRICS,
// ...) are deliberately not keywords: literals the keyword lists do not know
// are matched case-insensitively against any token, so they stay usable as
// identifiers.
const char *const SEMANTIC_VIEW_RULES[] = {
    "SemanticViewStatement <- CreateSemanticViewStmt / DropSemanticViewStmt / "
    "AlterSemanticViewStmt / "
    "DescribeSemanticViewStmt / ShowSemanticStmt",

    "CreateSemanticViewStmt <- 'CREATE' SemanticCreateModifier? 'SEMANTIC' "
    "'VIEW' IfNotExists? QualifiedName "
    "SemanticViewDefinitionBody",
    "SemanticCreateModifier <- 'OR' SemanticReplaceOrAlter",
    "SemanticReplaceOrAlter <- 'REPLACE' / 'ALTER'",

    "SemanticViewDefinitionBody <- SemanticTablesClause "
    "SemanticRelationshipsClause? SemanticFactsClause? "
    "SemanticDimensionsClause? SemanticMetricsClause? SemanticViewProperty*",

    "SemanticTablesClause <- 'TABLES' Parens(List(SemanticLogicalTable))",
    "SemanticLogicalTable <- SemanticTableAliasAs? SemanticTableSource "
    "SemanticTableOption*",
    "SemanticTableAliasAs <- ColId 'AS'",
    "SemanticTableSource <- SemanticParensSelect / BaseTableName",
    "SemanticParensSelect <- Parens(SelectStatementInternal)",
    "SemanticTableOption <- SemanticPrimaryKey / SemanticUnique / "
    "SemanticRangeConstraint / SemanticWithSynonyms / "
    "SemanticComment / SemanticTag",
    "SemanticPrimaryKey <- 'PRIMARY' 'KEY' SemanticColumnList",
    "SemanticUnique <- 'UNIQUE' SemanticColumnList",
    "SemanticRangeConstraint <- 'CONSTRAINT' SemanticConstraintName? "
    "'DISTINCT' 'RANGE' 'BETWEEN' ColId 'AND' ColId "
    "'EXCLUSIVE'",
    "SemanticConstraintName <- ColId",
    "SemanticColumnList <- Parens(List(ColId))",

    "SemanticRelationshipsClause <- 'RELATIONSHIPS' "
    "Parens(List(SemanticRelationship))",
    "SemanticRelationship <- SemanticRelationshipName? ColId "
    "SemanticColumnList 'REFERENCES' ColId "
    "SemanticReferencedColumns?",
    "SemanticRelationshipName <- ColId 'AS'",
    "SemanticReferencedColumns <- Parens(SemanticReferencedSpec)",
    "SemanticReferencedSpec <- SemanticRangeReference / "
    "SemanticReferencedColumnList",
    "SemanticRangeReference <- 'BETWEEN' ColId 'AND' ColId 'EXCLUSIVE'",
    "SemanticReferencedColumnList <- List(SemanticReferencedColumn)",
    "SemanticReferencedColumn <- SemanticAsof? ColId",
    "SemanticAsof <- 'ASOF'",

    "SemanticFactsClause <- 'FACTS' Parens(List(SemanticMember))",
    "SemanticDimensionsClause <- 'DIMENSIONS' Parens(List(SemanticMember))",
    "SemanticMetricsClause <- 'METRICS' Parens(List(SemanticMember))",
    "SemanticMember <- SemanticVisibility? SemanticMemberName SemanticLabels? "
    "SemanticUsing? SemanticNonAdditive? "
    "SemanticMetricFilter? SemanticMetricOffset? SemanticMetricCumulative? "
    "'AS' Expression SemanticMemberOption*",
    "SemanticVisibility <- 'PRIVATE' / 'PUBLIC'",
    "SemanticMemberName <- SemanticQualifiedMemberName / ColId",
    "SemanticQualifiedMemberName <- ColId '.' ColLabel",
    "SemanticLabels <- 'LABELS' '=' Parens(List(ColId))",
    "SemanticUsing <- 'USING' Parens(List(ColId))",
    "SemanticNonAdditive <- 'NON' 'ADDITIVE' 'BY' "
    "Parens(List(OrderByExpression))",
    "SemanticMetricFilter <- 'FILTER' Parens(SemanticMetricFilterBody)",
    "SemanticMetricFilterBody <- 'WHERE' Expression",
    "SemanticMetricOffset <- 'OFFSET' Parens(SemanticMetricOffsetBody)",
    "SemanticMetricOffsetBody <- SemanticDottedName 'BY' Expression",
    "SemanticMetricCumulative <- 'CUMULATIVE' Parens(SemanticCumulativeBody)",
    "SemanticCumulativeBody <- SemanticDottedName SemanticCumulativeFrame? "
    "SemanticCumulativePeriod?",
    "SemanticCumulativeFrame <- SemanticCumulativeWindow / "
    "SemanticCumulativeGrain",
    "SemanticCumulativeWindow <- 'OVER' Expression",
    "SemanticCumulativeGrain <- 'TO' ColLabel",
    "SemanticCumulativePeriod <- 'FIRST' / 'LAST' / 'AVERAGE'",
    "SemanticMemberOption <- SemanticWithSynonyms / SemanticComment / "
    "SemanticTag / SemanticSampleValues / "
    "SemanticIsEnum / SemanticCortexSearch",
    "SemanticWithSynonyms <- 'WITH'? 'SYNONYMS' '='? "
    "Parens(List(SemanticString))",
    "SemanticComment <- 'COMMENT' '=' SemanticString",
    "SemanticString <- StringLiteral+",
    "SemanticTag <- 'WITH'? 'TAG' Parens(List(SemanticTagEntry))",
    "SemanticTagEntry <- SemanticDottedName '=' SemanticString",
    "SemanticDottedName <- ColId ('.' ColLabel)*",
    "SemanticSampleValues <- 'SAMPLE_VALUES' Parens(List(SemanticString))",
    "SemanticIsEnum <- 'IS_ENUM'",
    "SemanticCortexSearch <- 'WITH' 'CORTEX' 'SEARCH' 'SERVICE' "
    "SemanticDottedName SemanticCortexSearchColumn?",
    "SemanticCortexSearchColumn <- 'USING' ColId",

    "SemanticViewProperty <- SemanticComment / SemanticMaxStaleness / "
    "SemanticAiSqlGeneration / "
    "SemanticAiQuestionCategorization / SemanticAiVerifiedQueries / "
    "SemanticTag / SemanticCopyGrants",
    "SemanticMaxStaleness <- 'MAX_STALENESS' '=' NumberLiteral",
    "SemanticAiSqlGeneration <- 'AI_SQL_GENERATION' SemanticString",
    "SemanticAiQuestionCategorization <- 'AI_QUESTION_CATEGORIZATION' "
    "SemanticString",
    "SemanticAiVerifiedQueries <- 'AI_VERIFIED_QUERIES' "
    "Parens(List(SemanticVerifiedQuery))",
    "SemanticVerifiedQuery <- ColId 'AS' "
    "Parens(SemanticVerifiedQueryProperty+)",
    "SemanticVerifiedQueryProperty <- SemanticVqQuestion / "
    "SemanticVqVerifiedAt / SemanticVqOnboarding / "
    "SemanticVqVerifiedBy / SemanticVqSql",
    "SemanticVqQuestion <- 'QUESTION' SemanticString",
    "SemanticVqVerifiedAt <- 'VERIFIED_AT' NumberLiteral",
    "SemanticVqOnboarding <- 'ONBOARDING_QUESTION' SemanticBoolean",
    "SemanticBoolean <- 'TRUE' / 'FALSE'",
    "SemanticVqVerifiedBy <- 'VERIFIED_BY' SemanticString",
    "SemanticVqSql <- 'SQL' SemanticString",
    "SemanticCopyGrants <- 'COPY' 'GRANTS'",

    "DropSemanticViewStmt <- 'DROP' 'SEMANTIC' 'VIEW' IfExists? QualifiedName",

    "AlterSemanticViewStmt <- 'ALTER' 'SEMANTIC' 'VIEW' IfExists? "
    "QualifiedName SemanticAlterAction",
    "SemanticAlterAction <- SemanticAlterRename / SemanticAlterSetComment / "
    "SemanticAlterUnsetComment / "
    "SemanticAlterSetMaxStaleness / SemanticAlterUnsetMaxStaleness / "
    "SemanticAlterAddMaterialization / "
    "SemanticAlterDropMaterialization / SemanticAlterRefreshMaterialization",
    "SemanticAlterRename <- 'RENAME' 'TO' QualifiedName",
    "SemanticAlterSetComment <- 'SET' 'COMMENT' '=' SemanticString",
    "SemanticAlterUnsetComment <- 'UNSET' 'COMMENT'",
    "SemanticAlterSetMaxStaleness <- 'SET' 'MAX_STALENESS' '=' NumberLiteral",
    "SemanticAlterUnsetMaxStaleness <- 'UNSET' 'MAX_STALENESS'",

    // Materializations: the body is a flat SEMANTIC_VIEW(...) item list
    // (DIMENSIONS ... METRICS ...)
    "SemanticAlterAddMaterialization <- 'ADD' 'MATERIALIZATION' IfNotExists? "
    "ColId SemanticMaterializationOption* "
    "'AS' SemanticQueryPiece*",
    "SemanticMaterializationOption <- SemanticMaterializationWarehouse / "
    "SemanticMaterializationRefreshMode / "
    "SemanticMaterializationStorage",
    "SemanticMaterializationStorage <- 'IN' QualifiedName",
    "SemanticMaterializationWarehouse <- 'WAREHOUSE' '=' ColId",
    "SemanticMaterializationRefreshMode <- 'REFRESH_MODE' '=' "
    "SemanticRefreshModeName",
    "SemanticRefreshModeName <- 'AUTO' / 'FULL' / 'INCREMENTAL'",
    "SemanticAlterDropMaterialization <- 'DROP' 'MATERIALIZATION' IfExists? "
    "ColId",
    "SemanticAlterRefreshMaterialization <- 'REFRESH' 'MATERIALIZATION' ColId "
    "SemanticRefreshModeName?",

    "DescribeSemanticViewStmt <- SemanticDescribeKeyword 'SEMANTIC' 'VIEW' "
    "QualifiedName",
    "SemanticDescribeKeyword <- 'DESCRIBE' / 'DESC'",

    "ShowSemanticStmt <- 'SHOW' 'SEMANTIC' SemanticShowTarget",
    "SemanticShowTarget <- SemanticShowViews / SemanticShowMaterializations / "
    "SemanticShowMembers",
    "SemanticShowViews <- 'VIEWS' SemanticShowLike? SemanticShowViewsScope?",
    "SemanticShowViewsScope <- 'IN' SemanticShowScopeKind QualifiedName",
    "SemanticShowScopeKind <- 'SCHEMA' / 'DATABASE'",
    "SemanticShowMaterializations <- 'MATERIALIZATIONS' SemanticShowLike? "
    "SemanticShowIn?",
    "SemanticShowMembers <- SemanticShowMemberKind SemanticShowLike? "
    "SemanticShowIn? SemanticShowForMetric?",
    "SemanticShowMemberKind <- 'DIMENSIONS' / 'METRICS' / 'FACTS'",
    "SemanticShowLike <- 'LIKE' StringLiteral",
    "SemanticShowIn <- 'IN' SemanticShowInView? QualifiedName",
    "SemanticShowInView <- 'SEMANTIC' 'VIEW'",
    "SemanticShowForMetric <- 'FOR' 'METRIC' SemanticDottedName",

    // (SHOW SEMANTIC ...) / (DESCRIBE SEMANTIC VIEW ...) as a table reference
    "SemanticShowTableRef <- Parens(SemanticShowOrDescribe) TableAlias?",
    "SemanticShowOrDescribe <- ShowSemanticStmt / DescribeSemanticViewStmt",

    // SEMANTIC_VIEW(...) in the FROM clause. The body is parsed as a flat
    // sequence of pieces; TransformSemanticViewRef assigns clause keywords
    // (DIMENSIONS / METRICS / FACTS parse as bare column references), items and
    // optional bare aliases. The PEG matcher ignores '!' lookaheads, so this is
    // how clause keywords stay unreserved.
    "SemanticViewRef <- SemanticViewKeyword Parens(SemanticViewQueryBody) "
    "TableAlias?",
    "SemanticViewKeyword <- 'SEMANTIC_VIEW' / SemanticViewTwoWords",
    "SemanticViewTwoWords <- 'SEMANTIC' 'VIEW'",
    "SemanticViewQueryBody <- QualifiedName SemanticQueryPiece*",
    "SemanticQueryPiece <- SemanticQueryComma / SemanticQueryAliasAs / "
    "SemanticQueryStar / SemanticQueryWhere / "
    "SemanticQueryExpression",
    "SemanticQueryComma <- ','",
    "SemanticQueryAliasAs <- 'AS' ColLabel",
    "SemanticQueryStar <- ColId '.' '*'",
    "SemanticQueryWhere <- 'WHERE' Expression",
    "SemanticQueryExpression <- Expression",

    // PARTITION BY EXCLUDING (window-function metrics) lowers to
    // __sv_excluding(...)
    "WindowPartitionExcluding <- 'EXCLUDING'",
};

const char *const WINDOW_PARTITION_RULE = "WindowPartition <- 'PARTITION' 'BY' WindowPartitionExcluding? "
                                          "List(Expression)";

//===--------------------------------------------------------------------===//
// Parse result helpers
//===--------------------------------------------------------------------===//
// Children are addressed by their position in the rule's sequence (noted above
// each transform).
ListParseResult &List(ParseResult &parse_result) {
	return parse_result.Cast<ListParseResult>();
}

//! The alternative matched by a rule whose body is an ordered choice
//! (List[Choice[alternative]])
ParseResult &Choice(ParseResult &parse_result) {
	if (parse_result.type == ParseResultType::CHOICE) {
		return parse_result.Cast<ChoiceParseResult>().GetResult();
	}
	return List(parse_result).Child<ChoiceParseResult>(0).GetResult();
}

bool Present(ListParseResult &list, idx_t index) {
	return list.Child<OptionalParseResult>(index).HasResult();
}

ParseResult &Optional(ListParseResult &list, idx_t index) {
	return list.Child<OptionalParseResult>(index).GetResult();
}

//! The elements of an `X*` (optional repeat) or `X+` (repeat) child
vector<reference<ParseResult>> Repeated(ParseResult &parse_result) {
	optional_ptr<ParseResult> current = &parse_result;
	if (current->type == ParseResultType::OPTIONAL) {
		auto &optional = current->Cast<OptionalParseResult>();
		if (!optional.HasResult()) {
			return {};
		}
		current = &optional.GetResult();
	}
	if (current->type == ParseResultType::LIST) {
		auto &list = current->Cast<ListParseResult>();
		if (list.GetChildren().size() == 1 && list.GetChild(0).type == ParseResultType::REPEAT) {
			current = &list.GetChild(0);
		}
	}
	vector<reference<ParseResult>> result;
	for (auto &child : current->Cast<RepeatParseResult>().GetChildren()) {
		result.push_back(child);
	}
	return result;
}

//! The elements of a `Parens(List(X))` child
vector<reference<ParseResult>> ParenthesizedList(ParseResult &parse_result) {
	return PEGTransformerFactory::ExtractParseResultsFromList(
	    PEGTransformerFactory::ExtractResultFromParens(parse_result));
}

template <class T>
vector<T> TransformAll(PEGTransformer &transformer, const vector<reference<ParseResult>> &parse_results) {
	vector<T> result;
	for (auto &parse_result : parse_results) {
		result.push_back(transformer.Transform<T>(parse_result.get()));
	}
	return result;
}

//! The keyword matched by a rule of keyword alternatives ('PRIVATE' / 'PUBLIC',
//! ...)
string Keyword(ParseResult &parse_result) {
	return StringUtil::Upper(Choice(parse_result).Cast<KeywordParseResult>().keyword);
}

bool Is(ParseResult &parse_result, const char *rule_name) {
	return StringUtil::CIEquals(parse_result.Name(), rule_name);
}

//! A `QualifiedName` child
SemanticQualifiedName Name(PEGTransformer &transformer, ListParseResult &list, idx_t index) {
	return SemanticQualifiedName::FromQualifiedName(transformer.Transform<QualifiedName>(list, index));
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
vector<string> TransformSemanticColumnList(PEGTransformer &transformer, ParseResult &parse_result) {
	return TransformAll<string>(transformer, ParenthesizedList(List(parse_result).GetChild(0)));
}

// 'WITH'? 'SYNONYMS' '='? Parens(List(SemanticString))
vector<string> TransformSemanticWithSynonyms(PEGTransformer &transformer, ParseResult &parse_result) {
	return TransformAll<string>(transformer, ParenthesizedList(List(parse_result).GetChild(3)));
}

// 'COMMENT' '=' SemanticString
string TransformSemanticComment(PEGTransformer &transformer, ParseResult &parse_result) {
	return transformer.Transform<string>(List(parse_result), 2);
}

// StringLiteral+ (adjacent string literals are concatenated, as in DuckDB's SQL
// parser)
string TransformSemanticString(PEGTransformer &transformer, ParseResult &parse_result) {
	string result;
	for (auto &literal : Repeated(List(parse_result).GetChild(0))) {
		result += transformer.Transform<string>(literal.get());
	}
	return result;
}

// 'WITH'? 'TAG' Parens(List(SemanticTagEntry))
vector<ParsedSemanticTag> TransformSemanticTag(PEGTransformer &transformer, ParseResult &parse_result) {
	return TransformAll<ParsedSemanticTag>(transformer, ParenthesizedList(List(parse_result).GetChild(2)));
}

// SemanticDottedName '=' SemanticString
ParsedSemanticTag TransformSemanticTagEntry(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = List(parse_result);
	ParsedSemanticTag tag;
	tag.name = StringUtil::Join(transformer.Transform<vector<string>>(list, 0), ".");
	tag.value = transformer.Transform<string>(list, 2);
	return tag;
}

// ColId ('.' ColLabel)*
vector<string> TransformSemanticDottedName(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = List(parse_result);
	vector<string> parts {transformer.Transform<string>(list, 0)};
	for (auto &part : Repeated(list.GetChild(1))) {
		parts.push_back(transformer.Transform<string>(List(part), 1));
	}
	return parts;
}

// ColId 'AS' (SemanticTableAliasAs, SemanticRelationshipName) / ColId
// (SemanticConstraintName)
string TransformSemanticName(PEGTransformer &transformer, ParseResult &parse_result) {
	return transformer.Transform<string>(List(parse_result), 0);
}

//===--------------------------------------------------------------------===//
// TABLES
//===--------------------------------------------------------------------===//
// 'TABLES' Parens(List(SemanticLogicalTable))
vector<ParsedSemanticTable> TransformSemanticTablesClause(PEGTransformer &transformer, ParseResult &parse_result) {
	return TransformAll<ParsedSemanticTable>(transformer, ParenthesizedList(List(parse_result).GetChild(1)));
}

// SemanticTableAliasAs? SemanticTableSource SemanticTableOption*
ParsedSemanticTable TransformSemanticLogicalTable(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = List(parse_result);
	ParsedSemanticTable table;
	transformer.TransformOptional<string>(list, 0, table.alias);
	table.source = transformer.Transform<unique_ptr<TableRef>>(list, 1);
	for (auto &option_node : Repeated(list.GetChild(2))) {
		auto &option = Choice(option_node);
		if (Is(option, "SemanticPrimaryKey")) {
			// 'PRIMARY' 'KEY' SemanticColumnList
			if (!table.primary_key.empty()) {
				throw ParserException("semantic view: logical table has more than one PRIMARY KEY");
			}
			table.primary_key = transformer.Transform<vector<string>>(List(option), 2);
		} else if (Is(option, "SemanticUnique")) {
			// 'UNIQUE' SemanticColumnList
			table.unique_keys.push_back(transformer.Transform<vector<string>>(List(option), 1));
		} else if (Is(option, "SemanticRangeConstraint")) {
			table.range_constraints.push_back(transformer.Transform<ParsedSemanticRangeConstraint>(option));
		} else if (Is(option, "SemanticWithSynonyms")) {
			table.synonyms = transformer.Transform<vector<string>>(option);
		} else if (Is(option, "SemanticComment")) {
			table.comment = transformer.Transform<string>(option);
		} else {
			table.tags = transformer.Transform<vector<ParsedSemanticTag>>(option);
		}
	}
	return table;
}

// SemanticParensSelect / BaseTableName
unique_ptr<TableRef> TransformSemanticTableSource(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &source = Choice(parse_result);
	if (Is(source, "BaseTableName")) {
		return transformer.Transform<unique_ptr<BaseTableRef>>(source);
	}
	// Parens(SelectStatementInternal)
	auto &select = PEGTransformerFactory::ExtractResultFromParens(List(source).GetChild(0));
	return make_uniq<SubqueryRef>(transformer.Transform<unique_ptr<SelectStatement>>(select));
}

// 'CONSTRAINT' SemanticConstraintName? 'DISTINCT' 'RANGE' 'BETWEEN' ColId 'AND'
// ColId 'EXCLUSIVE'
ParsedSemanticRangeConstraint TransformSemanticRangeConstraint(PEGTransformer &transformer, ParseResult &parse_result) {
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
                                                                        ParseResult &parse_result) {
	return TransformAll<ParsedSemanticRelationship>(transformer, ParenthesizedList(List(parse_result).GetChild(1)));
}

// SemanticRelationshipName? ColId SemanticColumnList 'REFERENCES' ColId
// SemanticReferencedColumns?
ParsedSemanticRelationship TransformSemanticRelationship(PEGTransformer &transformer, ParseResult &parse_result) {
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
	auto &referenced = PEGTransformerFactory::ExtractResultFromParens(List(Optional(list, 5)).GetChild(0));
	auto &spec = Choice(referenced);
	if (Is(spec, "SemanticRangeReference")) {
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
vector<ParsedSemanticMember> TransformSemanticMemberClause(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = List(parse_result);
	auto kind = MemberKind(list.Child<KeywordParseResult>(0).keyword);
	auto members = TransformAll<ParsedSemanticMember>(transformer, ParenthesizedList(list.GetChild(1)));
	for (auto &member : members) {
		member.kind = kind;
		if (member.is_private && kind == SemanticMemberKind::DIMENSION) {
			throw ParserException("semantic view: dimensions cannot be PRIVATE");
		}
		if (member.table.empty() && kind != SemanticMemberKind::METRIC) {
			throw ParserException("semantic view: %s '%s' must be qualified with its "
			                      "logical table (table.name)",
			                      StringUtil::Lower(SemanticMemberKindToString(kind)), member.name);
		}
	}
	return members;
}

// SemanticVisibility? SemanticMemberName SemanticLabels? SemanticUsing?
// SemanticNonAdditive? SemanticMetricFilter? SemanticMetricOffset?
// SemanticMetricCumulative? 'AS' Expression SemanticMemberOption*
ParsedSemanticMember TransformSemanticMember(PEGTransformer &transformer, ParseResult &parse_result) {
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
		auto &body = PEGTransformerFactory::ExtractResultFromParens(List(Optional(list, 5)).GetChild(1));
		member.filter = transformer.Transform<unique_ptr<ParsedExpression>>(List(body), 1);
	}
	if (Present(list, 6)) {
		// 'OFFSET' Parens(SemanticDottedName 'BY' Expression)
		auto &body = PEGTransformerFactory::ExtractResultFromParens(List(Optional(list, 6)).GetChild(1));
		auto &body_list = List(body);
		member.offset_dimension = transformer.Transform<vector<string>>(body_list, 0);
		member.offset_interval = transformer.Transform<unique_ptr<ParsedExpression>>(body_list, 2);
	}
	if (Present(list, 7)) {
		// 'CUMULATIVE' Parens(SemanticDottedName SemanticCumulativeFrame?
		// SemanticCumulativePeriod?)
		auto &body = PEGTransformerFactory::ExtractResultFromParens(List(Optional(list, 7)).GetChild(1));
		auto &body_list = List(body);
		member.cumulative_dimension = transformer.Transform<vector<string>>(body_list, 0);
		if (Present(body_list, 1)) {
			auto &frame = Choice(Optional(body_list, 1));
			if (Is(frame, "SemanticCumulativeWindow")) {
				// 'OVER' Expression
				member.cumulative_window = transformer.Transform<unique_ptr<ParsedExpression>>(List(frame), 1);
			} else {
				// 'TO' ColLabel
				member.cumulative_part = StringUtil::Lower(transformer.Transform<string>(List(frame), 1));
			}
		}
		if (Present(body_list, 2)) {
			auto period = Keyword(Optional(body_list, 2));
			member.cumulative_period = period == "FIRST"     ? SemanticCumulativePeriod::FIRST
			                           : period == "AVERAGE" ? SemanticCumulativePeriod::AVERAGE
			                                                 : SemanticCumulativePeriod::LAST;
		}
	}
	member.expression = transformer.Transform<unique_ptr<ParsedExpression>>(list, 9);
	for (auto &option_node : Repeated(list.GetChild(10))) {
		auto &option = Choice(option_node);
		if (Is(option, "SemanticWithSynonyms")) {
			member.synonyms = transformer.Transform<vector<string>>(option);
		} else if (Is(option, "SemanticComment")) {
			member.comment = transformer.Transform<string>(option);
		} else if (Is(option, "SemanticTag")) {
			member.tags = transformer.Transform<vector<ParsedSemanticTag>>(option);
		} else if (Is(option, "SemanticSampleValues")) {
			// 'SAMPLE_VALUES' Parens(List(SemanticString))
			member.sample_values = TransformAll<string>(transformer, ParenthesizedList(List(option).GetChild(1)));
		} else if (Is(option, "SemanticIsEnum")) {
			member.is_enum = true;
		} else {
			// 'WITH' 'CORTEX' 'SEARCH' 'SERVICE' SemanticDottedName ('USING' ColId)?
			auto &option_list = List(option);
			member.cortex_search_service = StringUtil::Join(transformer.Transform<vector<string>>(option_list, 4), ".");
			if (Present(option_list, 5)) {
				member.cortex_search_column = transformer.Transform<string>(List(Optional(option_list, 5)), 1);
			}
		}
	}
	return member;
}

// (ColId '.' ColLabel) / ColId
vector<string> TransformSemanticMemberName(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &name = Choice(parse_result);
	if (Is(name, "SemanticQualifiedMemberName")) {
		return {transformer.Transform<string>(List(name), 0), transformer.Transform<string>(List(name), 2)};
	}
	return {transformer.Transform<string>(name)};
}

//===--------------------------------------------------------------------===//
// CREATE SEMANTIC VIEW
//===--------------------------------------------------------------------===//
// ColId 'AS' Parens(SemanticVerifiedQueryProperty+)
ParsedSemanticVerifiedQuery TransformSemanticVerifiedQuery(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = List(parse_result);
	ParsedSemanticVerifiedQuery query;
	query.name = transformer.Transform<string>(list, 0);
	for (auto &property_node : Repeated(PEGTransformerFactory::ExtractResultFromParens(list.GetChild(2)))) {
		// each property is <keyword> <value>
		auto &property = Choice(property_node);
		auto &property_list = List(property);
		if (Is(property, "SemanticVqQuestion")) {
			query.question = transformer.Transform<string>(property_list, 1);
		} else if (Is(property, "SemanticVqVerifiedAt")) {
			query.verified_at = transformer.Transform<unique_ptr<ParsedExpression>>(property_list, 1)->ToString();
		} else if (Is(property, "SemanticVqOnboarding")) {
			query.onboarding_question = Keyword(property_list.GetChild(1)) == "TRUE";
		} else if (Is(property, "SemanticVqVerifiedBy")) {
			query.verified_by = transformer.Transform<string>(property_list, 1);
		} else {
			query.sql = transformer.Transform<string>(property_list, 1);
		}
	}
	return query;
}

void ApplyViewProperty(PEGTransformer &transformer, ParseResult &property, ParsedSemanticView &view) {
	if (Is(property, "SemanticComment")) {
		view.comment = transformer.Transform<string>(property);
	} else if (Is(property, "SemanticMaxStaleness")) {
		// 'MAX_STALENESS' '=' NumberLiteral
		auto value = transformer.Transform<unique_ptr<ParsedExpression>>(List(property), 2);
		view.has_max_staleness = true;
		view.max_staleness = value->Cast<ConstantExpression>().GetLiteral().ToValue().GetValue<int64_t>();
	} else if (Is(property, "SemanticAiSqlGeneration")) {
		view.ai_sql_generation = transformer.Transform<string>(List(property), 1);
	} else if (Is(property, "SemanticAiQuestionCategorization")) {
		view.ai_question_categorization = transformer.Transform<string>(List(property), 1);
	} else if (Is(property, "SemanticAiVerifiedQueries")) {
		// 'AI_VERIFIED_QUERIES' Parens(List(SemanticVerifiedQuery))
		view.verified_queries =
		    TransformAll<ParsedSemanticVerifiedQuery>(transformer, ParenthesizedList(List(property).GetChild(1)));
	} else if (Is(property, "SemanticTag")) {
		view.tags = transformer.Transform<vector<ParsedSemanticTag>>(property);
	} else {
		view.copy_grants = true;
	}
}

// SemanticTablesClause SemanticRelationshipsClause? SemanticFactsClause?
// SemanticDimensionsClause? SemanticMetricsClause? SemanticViewProperty*
unique_ptr<ParsedSemanticView> TransformSemanticViewDefinitionBody(PEGTransformer &transformer,
                                                                   ParseResult &parse_result) {
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

// 'CREATE' ('OR' ('REPLACE' / 'ALTER'))? 'SEMANTIC' 'VIEW' IfNotExists?
// QualifiedName SemanticViewDefinitionBody
unique_ptr<SQLStatement> TransformCreateSemanticViewStmt(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = List(parse_result);
	auto or_replace = Present(list, 1);
	auto if_not_exists = Present(list, 4);
	if (or_replace && if_not_exists) {
		throw ParserException("CREATE OR REPLACE SEMANTIC VIEW cannot be combined "
		                      "with IF NOT EXISTS");
	}
	auto view = transformer.Transform<unique_ptr<ParsedSemanticView>>(list, 6);
	view->name = Name(transformer, list, 5);
	auto on_conflict = or_replace      ? OnCreateConflict::REPLACE_ON_CONFLICT
	                   : if_not_exists ? OnCreateConflict::IGNORE_ON_CONFLICT
	                                   : OnCreateConflict::ERROR_ON_CONFLICT;
	return SemanticLowering::SelectAll(SemanticLowering::Create(*view, on_conflict));
}

//===--------------------------------------------------------------------===//
// DROP / ALTER / DESCRIBE / SHOW
//===--------------------------------------------------------------------===//
// 'DROP' 'SEMANTIC' 'VIEW' IfExists? QualifiedName
unique_ptr<SQLStatement> TransformDropSemanticViewStmt(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = List(parse_result);
	auto name = Name(transformer, list, 4);
	return SemanticLowering::SelectAll(SemanticLowering::Drop(name, Present(list, 3)));
}

void TransformQueryPieces(PEGTransformer &transformer, ParseResult &pieces, ParsedSemanticQuery &query);

SemanticRefreshMode RefreshMode(const string &keyword) {
	if (keyword == "FULL") {
		return SemanticRefreshMode::FULL;
	}
	if (keyword == "INCREMENTAL") {
		return SemanticRefreshMode::INCREMENTAL;
	}
	return SemanticRefreshMode::AUTO;
}

// 'ADD' 'MATERIALIZATION' IfNotExists? ColId SemanticMaterializationOption*
// 'AS' SemanticQueryPiece*
unique_ptr<SQLStatement> TransformAddMaterialization(PEGTransformer &transformer, ParseResult &action,
                                                     const SemanticQualifiedName &view, bool if_exists) {
	auto &list = List(action);
	ParsedSemanticMaterialization materialization;
	materialization.name = transformer.Transform<string>(list, 3);
	for (auto &option_node : Repeated(list.GetChild(4))) {
		auto &option = Choice(option_node);
		if (Is(option, "SemanticMaterializationWarehouse")) {
			// 'WAREHOUSE' '=' ColId
			materialization.warehouse = transformer.Transform<string>(List(option), 2);
		} else if (Is(option, "SemanticMaterializationStorage")) {
			// 'IN' QualifiedName: <database> or <database>.<schema>
			auto storage = Name(transformer, List(option), 1);
			if (!storage.catalog.empty()) {
				throw ParserException("ADD MATERIALIZATION ... IN expects <database> "
				                      "or <database>.<schema>");
			}
			bool has_schema = !storage.schema.empty();
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
unique_ptr<SQLStatement> TransformAlterSemanticViewStmt(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = List(parse_result);
	auto name = Name(transformer, list, 4);
	auto if_exists = Present(list, 3);
	auto &action = Choice(list.GetChild(5));
	if (Is(action, "SemanticAlterRename")) {
		// 'RENAME' 'TO' QualifiedName
		auto new_name = Name(transformer, List(action), 2);
		if (!new_name.catalog.empty() || !new_name.schema.empty()) {
			throw ParserException("ALTER SEMANTIC VIEW ... RENAME TO only accepts an unqualified name");
		}
		return SemanticLowering::SelectAll(SemanticLowering::Rename(name, if_exists, new_name.name));
	}
	if (Is(action, "SemanticAlterAddMaterialization")) {
		return TransformAddMaterialization(transformer, action, name, if_exists);
	}
	if (Is(action, "SemanticAlterDropMaterialization")) {
		// 'DROP' 'MATERIALIZATION' IfExists? ColId
		auto &action_list = List(action);
		return SemanticLowering::SelectAll(SemanticLowering::DropMaterialization(
		    name, if_exists, transformer.Transform<string>(action_list, 3), Present(action_list, 2)));
	}
	if (Is(action, "SemanticAlterRefreshMaterialization")) {
		// 'REFRESH' 'MATERIALIZATION' ColId ('AUTO' / 'FULL' / 'INCREMENTAL')?
		auto &action_list = List(action);
		Value mode(LogicalType::VARCHAR);
		if (Present(action_list, 3)) {
			mode = Value(Keyword(Optional(action_list, 3)));
		}
		return SemanticLowering::SelectAll(SemanticLowering::RefreshMaterialization(
		    name, if_exists, transformer.Transform<string>(action_list, 2), mode));
	}
	if (Is(action, "SemanticAlterSetMaxStaleness") || Is(action, "SemanticAlterUnsetMaxStaleness")) {
		// 'SET' 'MAX_STALENESS' '=' NumberLiteral / 'UNSET' 'MAX_STALENESS' (NULL)
		Value seconds(LogicalType::BIGINT);
		if (Is(action, "SemanticAlterSetMaxStaleness")) {
			auto value = transformer.Transform<unique_ptr<ParsedExpression>>(List(action), 3);
			seconds = Value::BIGINT(value->Cast<ConstantExpression>().GetLiteral().ToValue().GetValue<int64_t>());
		}
		return SemanticLowering::SelectAll(SemanticLowering::SetMaxStaleness(name, if_exists, seconds));
	}
	// 'SET' 'COMMENT' '=' SemanticString / 'UNSET' 'COMMENT' (a NULL comment)
	Value comment(LogicalType::VARCHAR);
	if (Is(action, "SemanticAlterSetComment")) {
		comment = Value(transformer.Transform<string>(List(action), 3));
	}
	return SemanticLowering::SelectAll(SemanticLowering::SetComment(name, if_exists, comment));
}

// ('DESCRIBE' / 'DESC') 'SEMANTIC' 'VIEW' QualifiedName
unique_ptr<TableRef> DescribeRef(PEGTransformer &transformer, ParseResult &parse_result) {
	return SemanticLowering::Describe(Name(transformer, List(parse_result), 3));
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

// 'SHOW' 'SEMANTIC' (SemanticShowViews / SemanticShowMaterializations /
// SemanticShowMembers)
unique_ptr<TableRef> ShowRef(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &target = Choice(List(parse_result).GetChild(2));
	auto &list = List(target);
	if (Is(target, "SemanticShowMaterializations")) {
		// 'MATERIALIZATIONS' SemanticShowLike? ('IN' ('SEMANTIC' 'VIEW')?
		// QualifiedName)?
		SemanticQualifiedName view;
		if (Present(list, 2)) {
			view = Name(transformer, List(Optional(list, 2)), 2);
		}
		return WithLike(transformer, SemanticLowering::ShowMaterializations(view), list, 1);
	}
	if (Is(target, "SemanticShowViews")) {
		// 'VIEWS' SemanticShowLike? ('IN' ('SCHEMA' / 'DATABASE') QualifiedName)?
		SemanticQualifiedName scope;
		if (Present(list, 2)) {
			auto &scope_list = List(Optional(list, 2));
			auto scope_name = Name(transformer, scope_list, 2);
			if (Keyword(scope_list.GetChild(1)) == "DATABASE") {
				scope.catalog = scope_name.name;
			} else {
				scope.catalog = scope_name.schema;
				scope.schema = scope_name.name;
			}
		}
		return WithLike(transformer, SemanticLowering::ShowViews(scope), list, 1);
	}
	// ('DIMENSIONS' / 'METRICS' / 'FACTS') SemanticShowLike? ('IN' ('SEMANTIC'
	// 'VIEW')? QualifiedName)?
	// ('FOR' 'METRIC' SemanticDottedName)?
	auto kind = MemberKind(Keyword(list.GetChild(0)));
	SemanticQualifiedName view;
	if (Present(list, 2)) {
		view = Name(transformer, List(Optional(list, 2)), 2);
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

unique_ptr<SQLStatement> TransformDescribeSemanticViewStmt(PEGTransformer &transformer, ParseResult &parse_result) {
	return SemanticLowering::SelectAll(DescribeRef(transformer, parse_result));
}

unique_ptr<SQLStatement> TransformShowSemanticStmt(PEGTransformer &transformer, ParseResult &parse_result) {
	return SemanticLowering::SelectAll(ShowRef(transformer, parse_result));
}

unique_ptr<SQLStatement> TransformSemanticViewStatement(PEGTransformer &transformer, ParseResult &parse_result) {
	return transformer.Transform<unique_ptr<SQLStatement>>(Choice(parse_result));
}

// Parens(ShowSemanticStmt / DescribeSemanticViewStmt) TableAlias?
unique_ptr<TableRef> TransformSemanticShowTableRef(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = List(parse_result);
	auto &statement = Choice(PEGTransformerFactory::ExtractResultFromParens(list.GetChild(0)));
	auto result =
	    Is(statement, "ShowSemanticStmt") ? ShowRef(transformer, statement) : DescribeRef(transformer, statement);
	ApplyAlias(transformer, list, 1, *result);
	return result;
}

//===--------------------------------------------------------------------===//
// SEMANTIC_VIEW(...)
//===--------------------------------------------------------------------===//
//! Assembles the flat SEMANTIC_VIEW(...) body. The PEG matcher has no negative
//! lookahead, so the clause keywords (DIMENSIONS / METRICS / FACTS parse as
//! bare column references), the items and their bare aliases are told apart
//! here: a clause keyword opens a clause, which takes one or more ','-separated
//! items.
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
			throw ParserException("SEMANTIC_VIEW requires at least one of "
			                      "DIMENSIONS, METRICS or FACTS");
		}
	}

private:
	static string BareName(const ParsedExpression &expression) {
		if (expression.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			return string();
		}
		auto &column = expression.Cast<ColumnRefExpression>();
		return column.IsQualified() ? string() : column.GetColumnName().GetIdentifierName();
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

	//! A clause keyword must be followed by at least one item, and a ',' by
	//! another item
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
void TransformQueryPieces(PEGTransformer &transformer, ParseResult &pieces, ParsedSemanticQuery &query) {
	SemanticQueryBodyBuilder builder(query);
	for (auto &piece_node : Repeated(pieces)) {
		auto &piece = Choice(piece_node);
		auto &piece_list = List(piece);
		if (Is(piece, "SemanticQueryComma")) {
			builder.Separator();
		} else if (Is(piece, "SemanticQueryAliasAs")) {
			// 'AS' ColLabel
			builder.AliasAs(transformer.Transform<string>(piece_list, 1));
		} else if (Is(piece, "SemanticQueryStar")) {
			// ColId '.' '*'
			builder.Wildcard(transformer.Transform<string>(piece_list, 0));
		} else if (Is(piece, "SemanticQueryWhere")) {
			// 'WHERE' Expression
			builder.Where(transformer.Transform<unique_ptr<ParsedExpression>>(piece_list, 1));
		} else {
			builder.Expression(transformer.Transform<unique_ptr<ParsedExpression>>(piece_list, 0));
		}
	}
	builder.Finish();
}

// QualifiedName SemanticQueryPiece*
unique_ptr<ParsedSemanticQuery> TransformSemanticViewQueryBody(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = List(parse_result);
	auto query = make_uniq<ParsedSemanticQuery>();
	query->view = Name(transformer, list, 0);
	TransformQueryPieces(transformer, list.GetChild(1), *query);
	return query;
}

// ('SEMANTIC_VIEW' / 'SEMANTIC' 'VIEW') Parens(SemanticViewQueryBody)
// TableAlias?
unique_ptr<TableRef> TransformSemanticViewRef(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = List(parse_result);
	auto &body = PEGTransformerFactory::ExtractResultFromParens(list.GetChild(1));
	auto result = SemanticLowering::Query(*transformer.Transform<unique_ptr<ParsedSemanticQuery>>(body));
	ApplyAlias(transformer, list, 2, *result);
	return result;
}

//===--------------------------------------------------------------------===//
// PARTITION BY [EXCLUDING]
//===--------------------------------------------------------------------===//
// 'PARTITION' 'BY' WindowPartitionExcluding? List(Expression)
vector<unique_ptr<ParsedExpression>> TransformWindowPartition(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = List(parse_result);
	vector<unique_ptr<ParsedExpression>> result;
	for (auto &expression : PEGTransformerFactory::ExtractParseResultsFromList(list.GetChild(3))) {
		result.push_back(transformer.Transform<unique_ptr<ParsedExpression>>(expression.get()));
	}
	if (Present(list, 2)) {
		// PARTITION BY EXCLUDING a, b -> PARTITION BY __sv_excluding(a, b)
		auto excluding = make_uniq<FunctionExpression>(SEMANTIC_EXCLUDING_FUNCTION, std::move(result));
		result.clear();
		result.push_back(std::move(excluding));
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Transform plumbing
//===--------------------------------------------------------------------===//
template <class T>
using rule_transform_t = T (*)(PEGTransformer &transformer, ParseResult &parse_result);

template <class T>
grammar_transform_process_function_t MakeTransform(rule_transform_t<T> transform) {
	return [transform](PEGTransformer &transformer, ParseResult &parse_result) -> arena_ptr<TransformProcess> {
		return transformer.MakeProcess<FinalizeTransformProcess>(
		    transformer, parse_result,
		    [transform](PEGTransformer &inner_transformer,
			            ParseResult &inner_result) -> arena_ptr<TransformResultValue> {
			    return inner_transformer.MakeResult<T>(transform(inner_transformer, inner_result));
		    });
	};
}

template <class T>
void SetTransform(vector<GrammarChange> &changes, const char *rule_name, rule_transform_t<T> transform) {
	changes.push_back(GrammarChange::SetTransformProcess(rule_name, MakeTransform<T>(transform)));
}

//===--------------------------------------------------------------------===//
// Activation
//===--------------------------------------------------------------------===//
//! Connections opened after LOAD parse with the grammar from the start
class SemanticViewsGrammarActivation final : public ExtensionCallback {
public:
	explicit SemanticViewsGrammarActivation(shared_ptr<SemanticViewsGrammarExtension> grammar_p)
	    : grammar(std::move(grammar_p)) {
	}

	void OnConnectionOpened(ClientContext &context) override {
		grammar->Activate(context);
	}

private:
	shared_ptr<SemanticViewsGrammarExtension> grammar;
};

//! Connections that were already open (including the one running LOAD) are
//! switched on their own thread once their current or next query ends, since
//! another thread may be parsing with their grammar right now
class SemanticViewsPendingGrammarActivation final : public ClientContextState {
public:
	static constexpr const char *KEY = "semantic_views_pending_grammar_activation";

	explicit SemanticViewsPendingGrammarActivation(shared_ptr<SemanticViewsGrammarExtension> grammar_p)
	    : grammar(std::move(grammar_p)) {
	}

	void QueryEnd(ClientContext &context) override {
		grammar->Activate(context);
		// One-shot: a later `SET active_grammar_extensions` by the user must stick
		context.registered_state->Remove(KEY);
	}

private:
	shared_ptr<SemanticViewsGrammarExtension> grammar;
};

} // namespace

//===--------------------------------------------------------------------===//
// Grammar extension
//===--------------------------------------------------------------------===//
SemanticViewsGrammarExtension::SemanticViewsGrammarExtension()
    : GrammarExtension(NAME, "CREATE / DROP / ALTER / DESCRIBE / SHOW SEMANTIC "
	                         "VIEW, SEMANTIC_VIEW(...) and PARTITION "
	                         "BY EXCLUDING") {
}

vector<GrammarChange> SemanticViewsGrammarExtension::GetChanges() const {
	vector<GrammarChange> changes;
	for (auto rule : SEMANTIC_VIEW_RULES) {
		changes.push_back(GrammarChange::AddRule(rule));
	}
	// Ahead of DuckDB's own statements and table references, which would
	// otherwise commit to a prefix: `DESCRIBE SEMANTIC ...` describing a table
	// named "semantic", `SEMANTIC_VIEW(...)` as a table function, ...
	changes.push_back(GrammarChange::PrependChoice("Statement", "SemanticViewStatement"));
	changes.push_back(GrammarChange::PrependChoice("ExplainableStatements", "SemanticViewStatement"));
	changes.push_back(GrammarChange::PrependChoice("InnerTableRef", "SemanticShowTableRef"));
	changes.push_back(GrammarChange::PrependChoice("InnerTableRef", "SemanticViewRef"));
	changes.push_back(GrammarChange::ReplaceRule(
	    WINDOW_PARTITION_RULE, MakeTransform<vector<unique_ptr<ParsedExpression>>>(TransformWindowPartition)));

	// statements and table references
	SetTransform(changes, "SemanticViewStatement", TransformSemanticViewStatement);
	SetTransform(changes, "CreateSemanticViewStmt", TransformCreateSemanticViewStmt);
	SetTransform(changes, "DropSemanticViewStmt", TransformDropSemanticViewStmt);
	SetTransform(changes, "AlterSemanticViewStmt", TransformAlterSemanticViewStmt);
	SetTransform(changes, "DescribeSemanticViewStmt", TransformDescribeSemanticViewStmt);
	SetTransform(changes, "ShowSemanticStmt", TransformShowSemanticStmt);
	SetTransform(changes, "SemanticShowTableRef", TransformSemanticShowTableRef);
	SetTransform(changes, "SemanticViewRef", TransformSemanticViewRef);
	SetTransform(changes, "SemanticViewQueryBody", TransformSemanticViewQueryBody);
	// definition
	SetTransform(changes, "SemanticViewDefinitionBody", TransformSemanticViewDefinitionBody);
	SetTransform(changes, "SemanticTablesClause", TransformSemanticTablesClause);
	SetTransform(changes, "SemanticLogicalTable", TransformSemanticLogicalTable);
	SetTransform(changes, "SemanticTableAliasAs", TransformSemanticName);
	SetTransform(changes, "SemanticTableSource", TransformSemanticTableSource);
	SetTransform(changes, "SemanticRangeConstraint", TransformSemanticRangeConstraint);
	SetTransform(changes, "SemanticConstraintName", TransformSemanticName);
	SetTransform(changes, "SemanticRelationshipsClause", TransformSemanticRelationshipsClause);
	SetTransform(changes, "SemanticRelationship", TransformSemanticRelationship);
	SetTransform(changes, "SemanticRelationshipName", TransformSemanticName);
	SetTransform(changes, "SemanticFactsClause", TransformSemanticMemberClause);
	SetTransform(changes, "SemanticDimensionsClause", TransformSemanticMemberClause);
	SetTransform(changes, "SemanticMetricsClause", TransformSemanticMemberClause);
	SetTransform(changes, "SemanticMember", TransformSemanticMember);
	SetTransform(changes, "SemanticMemberName", TransformSemanticMemberName);
	SetTransform(changes, "SemanticVerifiedQuery", TransformSemanticVerifiedQuery);
	// shared clauses
	SetTransform(changes, "SemanticColumnList", TransformSemanticColumnList);
	SetTransform(changes, "SemanticWithSynonyms", TransformSemanticWithSynonyms);
	SetTransform(changes, "SemanticComment", TransformSemanticComment);
	SetTransform(changes, "SemanticString", TransformSemanticString);
	SetTransform(changes, "SemanticTag", TransformSemanticTag);
	SetTransform(changes, "SemanticTagEntry", TransformSemanticTagEntry);
	SetTransform(changes, "SemanticDottedName", TransformSemanticDottedName);
	return changes;
}

shared_ptr<CompiledGrammar> SemanticViewsGrammarExtension::GetCompiledGrammar() {
	lock_guard<mutex> guard(grammar_lock);
	auto &compiled = compiled_grammars[NAME];
	if (!compiled) {
		vector<reference<GrammarExtension>> extensions;
		extensions.emplace_back(*this);
		compiled = CompiledGrammar::Create(extensions);
	}
	return compiled;
}

shared_ptr<CompiledGrammar> SemanticViewsGrammarExtension::GetCompiledGrammar(ClientContext &context,
                                                                              const vector<string> &active) {
	if (active.size() == 1) {
		return GetCompiledGrammar();
	}
	auto key = StringUtil::Join(active, ",");
	lock_guard<mutex> guard(grammar_lock);
	auto &compiled = compiled_grammars[key];
	if (!compiled) {
		compiled = CompiledGrammar::Create(context, active);
	}
	return compiled;
}

void SemanticViewsGrammarExtension::Activate(ClientContext &context) {
	auto &config = ClientConfig::GetConfig(context);
	auto &active = config.active_grammar_extensions;
	for (auto &name : active) {
		if (StringUtil::CIEquals(name, NAME)) {
			return;
		}
	}
	active.emplace_back(NAME);
	// Same effect as appending to `SET active_grammar_extensions`
	config.cached_grammar = GetCompiledGrammar(context, active);
}

vector<unique_ptr<SQLStatement>> SemanticViewsGrammarExtension::Parse(ClientContext &context, const string &sql) {
	auto grammar = ExtensionCallbackManager::Get(context).FindGrammarExtension(NAME);
	if (!grammar) {
		throw InternalException("semantic_views: the semantic_views grammar "
		                        "extension is not registered");
	}
	// Same parser settings as the connection
	auto options = ParserOptions::Builtin();
	options.identifier_case_mode = Settings::Get<PreserveIdentifierCaseSetting>(context);
	options.integer_division = Settings::Get<IntegerDivisionSetting>(context);
	options.regex_match_operator_semantics = Settings::Get<RegexMatchOperatorSemanticsSetting>(context);
	options.max_expression_depth = Settings::Get<MaxExpressionDepthSetting>(context);
	// The connection may have deactivated the grammar; this always parses with it
	options.compiled_grammar = static_cast<SemanticViewsGrammarExtension &>(*grammar).GetCompiledGrammar();
	options.extensions = nullptr;
	Parser parser(options);
	parser.ParseQuery(sql);
	return std::move(parser.statements);
}

void RegisterSemanticViewsGrammar(DatabaseInstance &db) {
	auto grammar = make_shared_ptr<SemanticViewsGrammarExtension>();
	GrammarExtension::Register(db, grammar);
	ExtensionCallback::Register(DBConfig::GetConfig(db), make_shared_ptr<SemanticViewsGrammarActivation>(grammar));
	for (auto &connection : db.GetConnectionManager().GetConnectionList()) {
		connection->registered_state->GetOrCreate<SemanticViewsPendingGrammarActivation>(
		    SemanticViewsPendingGrammarActivation::KEY, grammar);
	}
}

} // namespace duckdb
