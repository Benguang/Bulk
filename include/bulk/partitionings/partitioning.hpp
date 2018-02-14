#pragma once

#include <array>
#include <functional>
#include <numeric>
#include <vector>

#include "../util/binary_tree.hpp"
#include "../util/indices.hpp"

namespace bulk {
namespace experimental {

template <int D>
using index_type = std::array<int, D>;

/** Base class for partitionings over a 1D processor grid. */
template <int D>
class partitioning {
   public:
    /** Construct a partitioning for a global size. */
    partitioning(index_type<D> global_size)
        : global_size_(global_size) {}

    virtual ~partitioning() = default;

    /** Get the local and global sizes. */
    index_type<D> global_size() const { return global_size_; }
    int global_count() { return 0; }

    /** of an arbitrary processor */
    virtual index_type<D> local_size(int processor) = 0;

    /** the total count of elements on a processor */
    int local_count(int processor) {
        auto size = local_size(processor);
        return std::accumulate(size.begin(), size.end(), 1,
                               std::multiplies<int>());
    }

    /** Get the owner of a global index. */
    virtual int owner(index_type<D> xs) = 0;

    /** Convert indices between global and local. */
    virtual index_type<D> global_to_local(index_type<D> xs) = 0;
    virtual index_type<D> local_to_global(index_type<D> xs, int processor) = 0;

   protected:
    index_type<D> global_size_;
};

/** Base class for partitionings over a multi-dimensional processor grid */
template <int D, int G>
class multi_partitioning : public partitioning<D> {
   public:
    using partitioning<D>::local_size;

    multi_partitioning(index_type<D> global_size,
                       index_type<G> grid_size)
        : partitioning<D>(global_size), grid_size_(grid_size) {}

    virtual ~multi_partitioning() = default;

    /** Get the local and global sizes. */
    virtual index_type<D> local_size(index_type<G> processor) = 0;
    index_type<D> local_size(int processor) override {
        return local_size(util::unflatten<G>(grid_size_, processor));
    };

    /** Get the multi-dimensional owner of a global index. */
    virtual index_type<G> grid_owner(index_type<D> xs) = 0;
    int owner(index_type<D> xs) override {
        return util::flatten<G>(grid_size_, grid_owner(xs));
    }

    /** Convert indices between global and local. */
    virtual index_type<D> local_to_global(index_type<D> xs,
                                          index_type<G> processor) = 0;
    index_type<D> local_to_global(index_type<D> xs, int processor) override {
        return local_to_global(xs, util::unflatten<G>(grid_size_, processor));
    };

    index_type<G> grid() { return grid_size_; }

   protected:
    index_type<G> grid_size_;
};

/** Rectanglar partitionings over a multi-dimensional processor grid */
template <int D, int G>
class rectangular_partitioning : public multi_partitioning<D, G> {
   public:
    using multi_partitioning<D, G>::local_to_global;

    rectangular_partitioning(index_type<D> global_size,
                             index_type<G> grid_size)
        : multi_partitioning<D, G>(global_size, grid_size) {}

    /** Support origin queries by flattened, or multi-index */
    virtual index_type<D> origin(index_type<G> processor) const {
        return origin(util::flatten<G>(this->grid_size_, processor));
    }

    virtual index_type<D> origin(int processor) const {
        return origin(util::unflatten<G>(this->grid_size_, processor));
    }

    virtual index_type<D> local_to_global(index_type<D> xs,
                                          index_type<G> processor) {
        index_type<D> global = origin(processor);
        for (int d = 0; d < D; ++d) {
            global[d] += xs[d];
        }
        return global;
    }
};

}  // namespace experimental
}  // namespace bulk
