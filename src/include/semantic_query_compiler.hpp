//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_query_compiler.hpp
//
// Compiles a semantic query (dimensions / metrics / facts / filter) into a
// plain DuckDB SELECT statement:
//   - one aggregation block per metric home table, each joining only the
//     many-to-one tables it needs (no fan-out),
//   - blocks combined on the dimension values (UNION ALL + GROUP BY),
//   - metric post-aggregation expressions evaluated on top.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/statement/select_statement.hpp"
#include "semantic_analysis.hpp"

namespace duckdb {

class SemanticExpressionCompiler;
struct SemanticAliasGenerator;

struct SemanticQueryItem {
	SemanticMemberKind kind;
	//! Resolved expression (marker references)
	unique_ptr<ParsedExpression> resolved;
	string output_name;
};

struct SemanticQuerySpec {
	vector<SemanticQueryItem> items;
	//! Resolved filter, may be null
	unique_ptr<ParsedExpression> where;
};

class SemanticQueryCompiler {
public:
	SemanticQueryCompiler(const SemanticModel &model, ClientContext &context);

	//! Resolves the items and filter of a SEMANTIC_VIEW(...) clause (wildcards, names, PRIVATE checks)
	SemanticQuerySpec Resolve(const vector<ParsedSemanticQueryItem> &items, optional_ptr<ParsedExpression> where);
	//! Compiles a resolved query into a SELECT statement
	unique_ptr<SelectStatement> Compile(const SemanticQuerySpec &spec);
	//! Compiles a single member (including PRIVATE ones) for type probing; `column` is its output column
	unique_ptr<SelectStatement> CompileMemberProbe(idx_t member, idx_t &column);

private:
	unique_ptr<SelectStatement> CompileRowQuery(SemanticExpressionCompiler &compiler, SemanticAliasGenerator &aliases,
	                                            const SemanticQuerySpec &spec, bool has_dimensions, bool has_facts,
	                                            const set<idx_t> &row_tables);

	const SemanticModel &model;
	SemanticAnalyzer analyzer;
};

//! Binds a probe query per member and records its output type (called while creating a semantic view)
void ProbeSemanticModelTypes(ClientContext &context, SemanticModel &model);

} // namespace duckdb
