#pragma once

namespace duckdb {
namespace semantic_views_peg {

struct TransformResultValue {
	virtual ~TransformResultValue() = default;
};

template <class T>
struct TypedTransformResult : public TransformResultValue {
	explicit TypedTransformResult(T value_p) : value(std::move(value_p)) {
	}
	T value;
};

} // namespace semantic_views_peg
} // namespace duckdb
