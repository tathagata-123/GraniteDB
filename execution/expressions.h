#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../common/schema.h"
#include "../common/tuple.h"
#include "../common/value.h"

namespace simpledb {

enum class TupleSide {
    LEFT,
    RIGHT,
    SINGLE
};

inline bool IsNumericType(TypeId t) {
    return t == TypeId::INT32 || t == TypeId::INT64 || t == TypeId::DOUBLE;
}

inline double ValueToDouble(const Value &v) {
    switch (v.GetTypeId()) {
        case TypeId::INT32: return static_cast<double>(v.AsInt32());
        case TypeId::INT64: return static_cast<double>(v.AsInt64());
        case TypeId::DOUBLE: return v.AsDouble();
        default: throw std::runtime_error("Value is not numeric");
    }
}

inline int64_t ValueToInt64(const Value &v) {
    switch (v.GetTypeId()) {
        case TypeId::INT32: return static_cast<int64_t>(v.AsInt32());
        case TypeId::INT64: return v.AsInt64();
        default: throw std::runtime_error("Value is not integer-like");
    }
}

inline bool ValueAsBool(const Value &v) {
    if (v.IsNull()) return false;

    switch (v.GetTypeId()) {
        case TypeId::BOOLEAN: return v.AsBool();
        case TypeId::INT32: return v.AsInt32() != 0;
        case TypeId::INT64: return v.AsInt64() != 0;
        case TypeId::DOUBLE: return std::fabs(v.AsDouble()) > 1e-12;
        case TypeId::VARCHAR: return !v.AsString().empty();
        default: return false;
    }
}

inline int CompareValues(const Value &a, const Value &b) {
    if (a.IsNull() || b.IsNull()) {
        throw std::runtime_error("Cannot compare NULL values");
    }

    TypeId ta = a.GetTypeId();
    TypeId tb = b.GetTypeId();

    if (IsNumericType(ta) && IsNumericType(tb)) {
        double x = ValueToDouble(a);
        double y = ValueToDouble(b);
        if (x < y) return -1;
        if (x > y) return 1;
        return 0;
    }

    if (ta != tb) {
        throw std::runtime_error("Comparison between incompatible types");
    }

    switch (ta) {
        case TypeId::BOOLEAN: {
            bool x = a.AsBool(), y = b.AsBool();
            if (x < y) return -1;
            if (x > y) return 1;
            return 0;
        }
        case TypeId::VARCHAR: {
            if (a.AsString() < b.AsString()) return -1;
            if (a.AsString() > b.AsString()) return 1;
            return 0;
        }
        default:
            throw std::runtime_error("Unsupported comparison type");
    }
}

inline Value MakeArithmeticResult(double x, double y, char op, TypeId result_type) {
    if (result_type == TypeId::DOUBLE) {
        switch (op) {
            case '+': return Value(x + y);
            case '-': return Value(x - y);
            case '*': return Value(x * y);
            case '/':
                if (std::fabs(y) < 1e-12) throw std::runtime_error("Division by zero");
                return Value(x / y);
            default:
                throw std::runtime_error("Unknown arithmetic operator");
        }
    }

    int64_t xi = static_cast<int64_t>(x);
    int64_t yi = static_cast<int64_t>(y);

    switch (op) {
        case '+': return Value(static_cast<int64_t>(xi + yi));
        case '-': return Value(static_cast<int64_t>(xi - yi));
        case '*': return Value(static_cast<int64_t>(xi * yi));
        case '/':
            if (yi == 0) throw std::runtime_error("Division by zero");
            return Value(static_cast<int64_t>(xi / yi));
        default:
            throw std::runtime_error("Unknown arithmetic operator");
    }
}

inline std::string SerializeValueForHash(const Value &v) {
    std::ostringstream out;
    out << static_cast<int>(v.GetTypeId()) << "#";
    if (v.IsNull()) {
        out << "NULL";
        return out.str();
    }

    switch (v.GetTypeId()) {
        case TypeId::BOOLEAN: out << (v.AsBool() ? "1" : "0"); break;
        case TypeId::INT32: out << v.AsInt32(); break;
        case TypeId::INT64: out << v.AsInt64(); break;
        case TypeId::DOUBLE: out << v.AsDouble(); break;
        case TypeId::VARCHAR: out << v.AsString(); break;
        default: throw std::runtime_error("Unsupported value type for hash serialization");
    }
    return out.str();
}

class AbstractExpression {
public:
    virtual ~AbstractExpression() = default;

    virtual Value Evaluate(const Tuple *left_tuple,
                           const Schema *left_schema,
                           const Tuple *right_tuple,
                           const Schema *right_schema) const = 0;

    virtual AbstractExpression *Clone() const = 0;
};

class ConstantValueExpression : public AbstractExpression {
public:
    explicit ConstantValueExpression(Value value) : value_(std::move(value)) {}

    Value Evaluate(const Tuple *, const Schema *, const Tuple *, const Schema *) const override {
        return value_;
    }

    AbstractExpression *Clone() const override {
        return new ConstantValueExpression(value_);
    }

    const Value &GetValue() const { return value_; }

private:
    Value value_;
};

class ColumnValueExpression : public AbstractExpression {
public:
    ColumnValueExpression(TupleSide side, std::size_t column_idx)
        : side_(side), column_idx_(column_idx) {}

    TupleSide GetSide() const { return side_; }
    std::size_t GetColumnIdx() const { return column_idx_; }

    Value Evaluate(const Tuple *left_tuple,
                   const Schema *left_schema,
                   const Tuple *right_tuple,
                   const Schema *right_schema) const override {
        const Tuple *tuple = nullptr;
        const Schema *schema = nullptr;

        if (side_ == TupleSide::LEFT || side_ == TupleSide::SINGLE) {
            tuple = left_tuple;
            schema = left_schema;
        } else {
            tuple = right_tuple;
            schema = right_schema;
        }

        if (tuple == nullptr || schema == nullptr) {
            throw std::runtime_error("ColumnValueExpression received null tuple/schema");
        }
        if (column_idx_ >= schema->GetColumnCount()) {
            throw std::runtime_error("Column index out of range in ColumnValueExpression");
        }

        return tuple->GetValue(column_idx_);
    }

    AbstractExpression *Clone() const override {
        return new ColumnValueExpression(side_, column_idx_);
    }

private:
    TupleSide side_;
    std::size_t column_idx_;
};

enum class ComparisonType {
    EQ,
    NEQ,
    LT,
    LTE,
    GT,
    GTE
};

class ComparisonExpression : public AbstractExpression {
public:
    ComparisonExpression(ComparisonType cmp_type,
                         std::unique_ptr<AbstractExpression> left,
                         std::unique_ptr<AbstractExpression> right)
        : cmp_type_(cmp_type), left_(std::move(left)), right_(std::move(right)) {}

    ComparisonType GetComparisonType() const { return cmp_type_; }
    const AbstractExpression *GetLeft() const { return left_.get(); }
    const AbstractExpression *GetRight() const { return right_.get(); }

    Value Evaluate(const Tuple *left_tuple,
                   const Schema *left_schema,
                   const Tuple *right_tuple,
                   const Schema *right_schema) const override {
        Value lv = left_->Evaluate(left_tuple, left_schema, right_tuple, right_schema);
        Value rv = right_->Evaluate(left_tuple, left_schema, right_tuple, right_schema);

        if (lv.IsNull() || rv.IsNull()) {
            return Value(false);
        }

        int cmp = CompareValues(lv, rv);
        bool result = false;
        switch (cmp_type_) {
            case ComparisonType::EQ: result = (cmp == 0); break;
            case ComparisonType::NEQ: result = (cmp != 0); break;
            case ComparisonType::LT: result = (cmp < 0); break;
            case ComparisonType::LTE: result = (cmp <= 0); break;
            case ComparisonType::GT: result = (cmp > 0); break;
            case ComparisonType::GTE: result = (cmp >= 0); break;
        }
        return Value(result);
    }

    AbstractExpression *Clone() const override {
        return new ComparisonExpression(
            cmp_type_,
            std::unique_ptr<AbstractExpression>(left_->Clone()),
            std::unique_ptr<AbstractExpression>(right_->Clone()));
    }

private:
    ComparisonType cmp_type_;
    std::unique_ptr<AbstractExpression> left_;
    std::unique_ptr<AbstractExpression> right_;
};

class ConjunctionExpression : public AbstractExpression {
public:
    ConjunctionExpression(std::unique_ptr<AbstractExpression> left,
                          std::unique_ptr<AbstractExpression> right)
        : left_(std::move(left)), right_(std::move(right)) {}

    const AbstractExpression *GetLeft() const { return left_.get(); }
    const AbstractExpression *GetRight() const { return right_.get(); }

    Value Evaluate(const Tuple *left_tuple,
                   const Schema *left_schema,
                   const Tuple *right_tuple,
                   const Schema *right_schema) const override {
        bool l = ValueAsBool(left_->Evaluate(left_tuple, left_schema, right_tuple, right_schema));
        if (!l) return Value(false);
        bool r = ValueAsBool(right_->Evaluate(left_tuple, left_schema, right_tuple, right_schema));
        return Value(l && r);
    }

    AbstractExpression *Clone() const override {
        return new ConjunctionExpression(
            std::unique_ptr<AbstractExpression>(left_->Clone()),
            std::unique_ptr<AbstractExpression>(right_->Clone()));
    }

private:
    std::unique_ptr<AbstractExpression> left_;
    std::unique_ptr<AbstractExpression> right_;
};

class DisjunctionExpression : public AbstractExpression {
public:
    DisjunctionExpression(std::unique_ptr<AbstractExpression> left,
                          std::unique_ptr<AbstractExpression> right)
        : left_(std::move(left)), right_(std::move(right)) {}

    const AbstractExpression *GetLeft() const { return left_.get(); }
    const AbstractExpression *GetRight() const { return right_.get(); }

    Value Evaluate(const Tuple *left_tuple,
                   const Schema *left_schema,
                   const Tuple *right_tuple,
                   const Schema *right_schema) const override {
        bool l = ValueAsBool(left_->Evaluate(left_tuple, left_schema, right_tuple, right_schema));
        if (l) return Value(true);
        bool r = ValueAsBool(right_->Evaluate(left_tuple, left_schema, right_tuple, right_schema));
        return Value(r);
    }

    AbstractExpression *Clone() const override {
        return new DisjunctionExpression(
            std::unique_ptr<AbstractExpression>(left_->Clone()),
            std::unique_ptr<AbstractExpression>(right_->Clone()));
    }

private:
    std::unique_ptr<AbstractExpression> left_;
    std::unique_ptr<AbstractExpression> right_;
};

enum class ArithmeticType {
    ADD,
    SUB,
    MUL,
    DIV
};

class ArithmeticExpression : public AbstractExpression {
public:
    ArithmeticExpression(ArithmeticType arith_type,
                         std::unique_ptr<AbstractExpression> left,
                         std::unique_ptr<AbstractExpression> right)
        : arith_type_(arith_type), left_(std::move(left)), right_(std::move(right)) {}

    const AbstractExpression *GetLeft() const { return left_.get(); }
    const AbstractExpression *GetRight() const { return right_.get(); }

    Value Evaluate(const Tuple *left_tuple,
                   const Schema *left_schema,
                   const Tuple *right_tuple,
                   const Schema *right_schema) const override {
        Value lv = left_->Evaluate(left_tuple, left_schema, right_tuple, right_schema);
        Value rv = right_->Evaluate(left_tuple, left_schema, right_tuple, right_schema);

        if (lv.IsNull() || rv.IsNull()) {
            return Value::Null(TypeId::DOUBLE);
        }
        if (!IsNumericType(lv.GetTypeId()) || !IsNumericType(rv.GetTypeId())) {
            throw std::runtime_error("Arithmetic requires numeric operands");
        }

        TypeId result_type =
            (arith_type_ == ArithmeticType::DIV || lv.GetTypeId() == TypeId::DOUBLE || rv.GetTypeId() == TypeId::DOUBLE)
                ? TypeId::DOUBLE
                : TypeId::INT64;

        char op = '+';
        switch (arith_type_) {
            case ArithmeticType::ADD: op = '+'; break;
            case ArithmeticType::SUB: op = '-'; break;
            case ArithmeticType::MUL: op = '*'; break;
            case ArithmeticType::DIV: op = '/'; break;
        }

        return MakeArithmeticResult(ValueToDouble(lv), ValueToDouble(rv), op, result_type);
    }

    AbstractExpression *Clone() const override {
        return new ArithmeticExpression(
            arith_type_,
            std::unique_ptr<AbstractExpression>(left_->Clone()),
            std::unique_ptr<AbstractExpression>(right_->Clone()));
    }

private:
    ArithmeticType arith_type_;
    std::unique_ptr<AbstractExpression> left_;
    std::unique_ptr<AbstractExpression> right_;
};

}  // namespace simpledb
