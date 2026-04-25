#ifndef ANN_HELPER_TYPE_H
#define ANN_HELPER_TYPE_H

#include <boost/preprocessor/seq.hpp>
#include <boost/preprocessor/repeat.hpp>
#include <boost/preprocessor/repetition/repeat_from_to.hpp>

#include "c.h"

namespace ann_helper {
namespace internal {
template<uint16 N> struct marker_id { static uint16 const value = N; };
template<typename T> struct marker_type { typedef T type; };
template<typename T, uint16 N>
struct register_id : marker_id<N>, marker_type<T> {
private:
    friend marker_type<T> marked_id(marker_id<N>) {
        return marker_type<T>();
    }
};
} /* namespace internal */

struct UnknownType {};

#define TYPES (UnknownType)(float)(uint8)(uint32)(size_t)(char)

#define NUM_TYPES BOOST_PP_SEQ_SIZE(TYPES)
template<typename T> struct TypeIdMap : internal::register_id<T, __UINT16_MAX__> {};
#define SET_TYPE(r, data, i, elem) template<> struct TypeIdMap<elem> : internal::register_id<elem, i> {};
BOOST_PP_SEQ_FOR_EACH_I(SET_TYPE, ~, TYPES)
#undef SET_TYPE
#define GET_TYPE_ID(type) TypeIdMap<type>::value
#define GET_TYPE(id) BOOST_PP_SEQ_ELEM(id, TYPES)
} /* namespace ann_helper */

#endif /* ANN_HELPER_TYPE_H */
