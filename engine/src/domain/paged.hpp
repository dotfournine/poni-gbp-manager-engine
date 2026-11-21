// One page of a list as the API returns it: the rows of the page, and how
// many rows the filter matches in all. Every list in domain/models.hpp is a
// Paged<T> under its own name (`using ClientList = Paged<Client>;`).
#pragma once

#include <boost/describe/class.hpp>

#include <cstdint>
#include <vector>

namespace poni::model {

template <class T>
struct Paged {
    std::vector<T> items;
    std::int64_t total = 0;

    // Inside the class: BOOST_DESCRIBE_STRUCT cannot describe a template.
    BOOST_DESCRIBE_CLASS(Paged, (), (items, total), (), ())
};

}  // namespace poni::model
