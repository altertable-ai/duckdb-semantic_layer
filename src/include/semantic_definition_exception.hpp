#pragma once

#include "duckdb/common/exception/binder_exception.hpp"

namespace duckdb {

//! An incompatible semantic definition, distinguished from failures to bind physical sources or access storage.
class SemanticDefinitionException : public BinderException {
public:
	using BinderException::BinderException;
};

} // namespace duckdb
