//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_view_peg_context.hpp
//
// Thread-local bridge from the PEG transformer to SemanticViewParser::ParseDefinition:
// when set, TransformCreateSemanticViewStmt stores the parsed definition here
// (in addition to lowering the statement).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "parsed_semantic_view.hpp"

namespace duckdb {

struct SemanticViewPegContext {
	static thread_local unique_ptr<ParsedSemanticView> *definition_capture;

	struct ScopedCapture {
		explicit ScopedCapture(unique_ptr<ParsedSemanticView> &target) : previous(definition_capture) {
			definition_capture = &target;
		}
		~ScopedCapture() {
			definition_capture = previous;
		}
		unique_ptr<ParsedSemanticView> *previous;
	};
};

} // namespace duckdb
