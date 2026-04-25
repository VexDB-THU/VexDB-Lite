#ifndef VEX_FILTER_HPP
#define VEX_FILTER_HPP

#include "vex/vex_types.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace vex {

inline uint64_t FNV1aHash(const char *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= static_cast<uint8_t>(data[i]);
        h *= 0x100000001b3ULL;
    }
    return h;
}

struct MetaColumnDesc {
    TypeId type_id = TypeId::INVALID;
    uint32_t offset = 0;
    uint32_t size = 0;

    static uint32_t GetTypeSize(TypeId type) {
        switch (type) {
        case TypeId::BOOLEAN:
        case TypeId::INT8:
        case TypeId::UINT8:
            return 1;
        case TypeId::INT16:
        case TypeId::UINT16:
            return 2;
        case TypeId::INT32:
        case TypeId::UINT32:
        case TypeId::FLOAT32:
            return 4;
        case TypeId::INT64:
        case TypeId::UINT64:
        case TypeId::FLOAT64:
            return 8;
        default:
            return 8;
        }
    }
};

class FilterPredicate {
public:
    virtual ~FilterPredicate() = default;
    virtual std::unique_ptr<FilterPredicate> Clone() const = 0;
    virtual bool Matches(const uint8_t *meta_data) const = 0;
    virtual double Selectivity() const { return 1.0; }
};

class EqualityFilter : public FilterPredicate {
public:
    uint32_t offset;
    uint32_t size;
    std::vector<uint8_t> value;
    double selectivity_;

    EqualityFilter(uint32_t offset_p, uint32_t size_p, const void *val, double sel = 0.1)
        : offset(offset_p), size(size_p), value(size_p), selectivity_(sel) {
        std::memcpy(value.data(), val, size);
    }

    std::unique_ptr<FilterPredicate> Clone() const override {
        return std::unique_ptr<FilterPredicate>(new EqualityFilter(*this));
    }

    bool Matches(const uint8_t *meta_data) const override {
        return std::memcmp(meta_data + offset, value.data(), size) == 0;
    }

    double Selectivity() const override {
        return selectivity_;
    }
};

class RangeFilter : public FilterPredicate {
public:
    uint32_t offset;
    uint32_t size;
    TypeId type_id;
    std::vector<uint8_t> min_val;
    std::vector<uint8_t> max_val;
    bool has_min;
    bool has_max;
    double selectivity_;

    RangeFilter(uint32_t offset_p, uint32_t size_p, TypeId type_id_p, double sel = 0.3)
        : offset(offset_p), size(size_p), type_id(type_id_p), has_min(false), has_max(false), selectivity_(sel) {
    }

    std::unique_ptr<FilterPredicate> Clone() const override {
        return std::unique_ptr<FilterPredicate>(new RangeFilter(*this));
    }

    void SetMin(const void *val) {
        min_val.resize(size);
        std::memcpy(min_val.data(), val, size);
        has_min = true;
    }

    void SetMax(const void *val) {
        max_val.resize(size);
        std::memcpy(max_val.data(), val, size);
        has_max = true;
    }

    bool Matches(const uint8_t *meta_data) const override {
        const uint8_t *data = meta_data + offset;
        if (has_min && CompareValues(data, min_val.data()) < 0) {
            return false;
        }
        if (has_max && CompareValues(data, max_val.data()) > 0) {
            return false;
        }
        return true;
    }

    double Selectivity() const override {
        return selectivity_;
    }

private:
    int CompareValues(const uint8_t *a, const uint8_t *b) const {
        switch (type_id) {
        case TypeId::INT32: {
            int32_t va;
            int32_t vb;
            std::memcpy(&va, a, sizeof(int32_t));
            std::memcpy(&vb, b, sizeof(int32_t));
            return va < vb ? -1 : (va > vb ? 1 : 0);
        }
        case TypeId::INT64: {
            int64_t va;
            int64_t vb;
            std::memcpy(&va, a, sizeof(int64_t));
            std::memcpy(&vb, b, sizeof(int64_t));
            return va < vb ? -1 : (va > vb ? 1 : 0);
        }
        case TypeId::FLOAT32: {
            float va;
            float vb;
            std::memcpy(&va, a, sizeof(float));
            std::memcpy(&vb, b, sizeof(float));
            return va < vb ? -1 : (va > vb ? 1 : 0);
        }
        case TypeId::FLOAT64: {
            double va;
            double vb;
            std::memcpy(&va, a, sizeof(double));
            std::memcpy(&vb, b, sizeof(double));
            return va < vb ? -1 : (va > vb ? 1 : 0);
        }
        default:
            return std::memcmp(a, b, size);
        }
    }
};

class InListFilter : public FilterPredicate {
public:
    uint32_t offset;
    uint32_t size;
    std::vector<std::vector<uint8_t>> values;
    double selectivity_;

    InListFilter(uint32_t offset_p, uint32_t size_p, double sel = 0.1)
        : offset(offset_p), size(size_p), selectivity_(sel) {
    }

    std::unique_ptr<FilterPredicate> Clone() const override {
        return std::unique_ptr<FilterPredicate>(new InListFilter(*this));
    }

    void AddValue(const void *val) {
        std::vector<uint8_t> v(size);
        std::memcpy(v.data(), val, size);
        values.push_back(std::move(v));
    }

    bool Matches(const uint8_t *meta_data) const override {
        const uint8_t *data = meta_data + offset;
        for (const auto &v : values) {
            if (std::memcmp(data, v.data(), size) == 0) {
                return true;
            }
        }
        return false;
    }

    double Selectivity() const override {
        return selectivity_;
    }
};

class ConjunctionFilter : public FilterPredicate {
public:
    std::vector<std::unique_ptr<FilterPredicate>> children;

    void AddChild(std::unique_ptr<FilterPredicate> child) {
        children.push_back(std::move(child));
    }

    std::unique_ptr<FilterPredicate> Clone() const override {
        auto result = std::unique_ptr<ConjunctionFilter>(new ConjunctionFilter());
        for (const auto &child : children) {
            result->AddChild(child->Clone());
        }
        return result;
    }

    bool Matches(const uint8_t *meta_data) const override {
        for (const auto &child : children) {
            if (!child->Matches(meta_data)) {
                return false;
            }
        }
        return true;
    }

    double Selectivity() const override {
        double sel = 1.0;
        for (const auto &child : children) {
            sel *= child->Selectivity();
        }
        return sel;
    }
};

} // namespace vex

#endif // VEX_FILTER_HPP
