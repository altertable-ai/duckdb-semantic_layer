#pragma once

namespace duckdb {
namespace semantic_views_peg {
struct TransformEnumValue {
	virtual ~TransformEnumValue() = default;
};

template <class T>
struct TypedTransformEnumResult : public TransformEnumValue {
	explicit TypedTransformEnumResult(T value_p) : value(std::move(value_p)) {
	}
	T value;
};

} // namespace semantic_views_peg
} // namespace duckdb
