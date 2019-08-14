
//
// This source file is part of appleseed.
// Visit https://appleseedhq.net/ for additional information and resources.
//
// This software is released under the MIT license.
//
// Copyright (c) 2019 Stephen Agyemang, The appleseedhq Organization
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

// Interface header.
#include "sdtree.h"

// appleseed.renderer headers.
#include "renderer/global/globallogger.h"

// appleseed.foundation headers.
#include "foundation/math/scalar.h"
#include "foundation/math/sampling/mappings.h"
#include "foundation/utility/string.h"

// Standard headers.
#include <cmath>

using namespace foundation;

namespace renderer
{

//
// SD-Tree implementation for "Practical Path Guiding for Efficient Light-Transport Simulation" [Müller et al. 2017].
//

const float SDTreeEpsilon = 1e-4f;
const size_t SpatialSubdivisionThreshold = 4000; // TODO: make this dependent on the filter types
const float DTreeThreshold = 0.01;
const size_t DTreeMaxDepth = 20;

// Sampling fraction optimization constants.

const float Beta1 = 0.9f;
const float Beta2 = 0.999f;
const float OptimizationEpsilon = 1e-8f;
const float Regularization = 0.01f;

void atomic_add(
    std::atomic<float>&                 atomic,
    const float                         value)
{
    float current = atomic.load(std::memory_order_relaxed);
    while (!atomic.compare_exchange_weak(current, current + value))
        ;
}

inline float logistic(float x)
{
    return 1.0f / (1.0f + std::exp(-x));
}

Vector3f cylindrical_to_cartesian(
    const Vector2f&                     cylindrical_direction)
{
    return sample_sphere_uniform(cylindrical_direction);
}

Vector2f cartesian_to_cylindrical(
    const Vector3f&                     direction)
{
    // TODO: Handle float imprecision

    const float cosTheta = direction.y;
    float phi = std::atan2(direction.z, direction.x);

    if (phi < 0.0f)
        phi = std::max(phi + TwoPi<float>(), 0.0f);

    return Vector2f(phi * RcpTwoPi<float>(), 1.0f - 0.5f * (cosTheta + 1.0f));
}


QuadTreeNode::QuadTreeNode(
    const bool                          create_children,
    const float                         radiance_sum)
    : m_is_leaf(!create_children)
    , m_current_iter_radiance_sum(radiance_sum)
    , m_previous_iter_radiance_sum(radiance_sum)
{   
    if(create_children)
    {
        m_upper_left_node.reset(new QuadTreeNode(false));
        m_upper_right_node.reset(new QuadTreeNode(false));
        m_lower_right_node.reset(new QuadTreeNode(false));
        m_lower_left_node.reset(new QuadTreeNode(false));
    }
}

QuadTreeNode::QuadTreeNode(const QuadTreeNode& other)
    : m_current_iter_radiance_sum(other.m_current_iter_radiance_sum.load(std::memory_order_relaxed))
    , m_previous_iter_radiance_sum(other.m_previous_iter_radiance_sum)
    , m_is_leaf(other.m_is_leaf)
{   
    if(!other.m_is_leaf)
    {
        m_upper_left_node.reset(new QuadTreeNode(*other.m_upper_left_node));
        m_upper_right_node.reset(new QuadTreeNode(*other.m_upper_right_node));
        m_lower_right_node.reset(new QuadTreeNode(*other.m_lower_right_node));
        m_lower_left_node.reset(new QuadTreeNode(*other.m_lower_left_node));
    }
}

void QuadTreeNode::add_radiance(
    Vector2f&                           direction,
    const float                         radiance)
{
    if(m_is_leaf)
        atomic_add(m_current_iter_radiance_sum, radiance);
    else
        choose_node(direction)->add_radiance(direction, radiance);
}

void QuadTreeNode::add_radiance(
    const AABB2f&                       splat_aabb,
    const AABB2f&                       node_aabb,
    const float                         radiance)
{
    const AABB2f intersection_aabb(AABB2f::intersect(splat_aabb, node_aabb));

    if(!intersection_aabb.is_valid())
        return;

    const float intersection_volume = intersection_aabb.volume();

    if(intersection_volume <= 0.0f)
        return;

    if(m_is_leaf)
    {
        atomic_add(m_current_iter_radiance_sum, radiance * intersection_volume);
    }
    else
    {
        const Vector2f node_size = node_aabb.extent();
        AABB2f child_aabb(node_aabb.min, node_aabb.min + 0.5f * node_size);
        m_upper_left_node->add_radiance(splat_aabb, child_aabb, radiance);
        
        child_aabb.translate(Vector2f(0.5f * node_size.x, 0.0f));
        m_upper_right_node->add_radiance(splat_aabb, child_aabb, radiance);

        child_aabb.translate(Vector2f(0.0f, 0.5f * node_size.x));
        m_lower_right_node->add_radiance(splat_aabb, child_aabb, radiance);

        child_aabb.translate(Vector2f(-0.5f * node_size.x, 0.0f));
        m_lower_left_node->add_radiance(splat_aabb, child_aabb, radiance);
    }
}

size_t QuadTreeNode::max_depth() const
{
    if(m_is_leaf)
        return 1;
    
    size_t max_child_depth = m_upper_left_node->max_depth();
    max_child_depth = std::max(m_upper_right_node->max_depth(), max_child_depth);
    max_child_depth = std::max(m_lower_right_node->max_depth(), max_child_depth);
    max_child_depth = std::max(m_lower_left_node->max_depth(), max_child_depth);
    return 1 + max_child_depth;
}

size_t QuadTreeNode::node_count() const
{
    if(m_is_leaf)
        return 1;
    
    return 1
        + m_upper_left_node->node_count()
        + m_upper_right_node->node_count()
        + m_lower_right_node->node_count()
        + m_lower_left_node->node_count();
}

float QuadTreeNode::radiance_sum() const
{
    return m_previous_iter_radiance_sum;
}

float QuadTreeNode::build_radiance_sums()
{
    if(m_is_leaf)
    {
        m_previous_iter_radiance_sum = m_current_iter_radiance_sum.load(std::memory_order_relaxed);
        return m_previous_iter_radiance_sum;
    }

    m_previous_iter_radiance_sum = 0.0f;
    m_previous_iter_radiance_sum += m_upper_left_node->build_radiance_sums();
    m_previous_iter_radiance_sum += m_upper_right_node->build_radiance_sums();
    m_previous_iter_radiance_sum += m_lower_right_node->build_radiance_sums();
    m_previous_iter_radiance_sum += m_lower_left_node->build_radiance_sums();
    return m_previous_iter_radiance_sum;
}

void QuadTreeNode::restructure(
    const float                         total_radiance_sum,
    const float                         subdiv_threshold,
    const size_t                        depth)
{   
    if(total_radiance_sum <= 0.0f) // TODO: Should we still grow?
        return;

    const float fraction = m_previous_iter_radiance_sum / total_radiance_sum;

    if(fraction > subdiv_threshold && depth < DTreeMaxDepth)
    {
        if(m_is_leaf)
        {
            m_is_leaf = false;
            const float quarter_sum = 0.25f * m_previous_iter_radiance_sum;
            m_upper_left_node.reset(new QuadTreeNode(false, quarter_sum));
            m_upper_right_node.reset(new QuadTreeNode(false, quarter_sum));
            m_lower_right_node.reset(new QuadTreeNode(false, quarter_sum));
            m_lower_left_node.reset(new QuadTreeNode(false, quarter_sum));
        }
        m_upper_left_node->restructure(total_radiance_sum, subdiv_threshold, depth + 1);
        m_upper_right_node->restructure(total_radiance_sum, subdiv_threshold, depth + 1);
        m_lower_right_node->restructure(total_radiance_sum, subdiv_threshold, depth + 1);
        m_lower_left_node->restructure(total_radiance_sum, subdiv_threshold, depth + 1);            
    }
    else if(!m_is_leaf)
    {
        m_is_leaf = true;
        m_upper_left_node.reset(nullptr);
        m_upper_right_node.reset(nullptr);
        m_lower_right_node.reset(nullptr);
        m_lower_left_node.reset(nullptr);
    }

    m_current_iter_radiance_sum.store(0.0f, std::memory_order_relaxed);
}

const Vector2f QuadTreeNode::sample(
    Vector2f&                           sample,
    float&                              pdf) const
{
    assert(sample.x >= 0.0f && sample.x < 1.0f);
    assert(sample.y >= 0.0f && sample.y < 1.0f);

    if(sample.x >= 1.0f)
        sample.x = std::nextafter(1.0f, 0.0f);

    if(sample.y >= 1.0f)
        sample.y = std::nextafter(1.0f, 0.0f);

    if(m_is_leaf)
    {
        pdf *= RcpFourPi<float>();
        return sample;
    }

    const float upper_left = m_upper_left_node->m_previous_iter_radiance_sum;
    const float upper_right = m_upper_right_node->m_previous_iter_radiance_sum;
    const float lower_right = m_lower_right_node->m_previous_iter_radiance_sum;
    const float lower_left = m_lower_left_node->m_previous_iter_radiance_sum;
    const float sum_left_half = upper_left + lower_left;
    const float sum_right_half = upper_right + lower_right;

    // TODO: Handle floating point imprecision

    float factor = sum_left_half / m_previous_iter_radiance_sum;

    if(sample.x < factor)
    {
        sample.x /= factor;
        factor = upper_left / sum_left_half;

        if(sample.y < factor)
        {
            sample.y /= factor;
            const Vector2f sampled_direction =
                Vector2f(0.0f, 0.0f) + 0.5f * m_upper_left_node->sample(sample, pdf);

            const float probability_factor = 4.0f * upper_left / m_previous_iter_radiance_sum;
            pdf *= probability_factor;
            return sampled_direction;
        }

        sample.y = (sample.y - factor) / (1.0f - factor);
        const Vector2f sampled_direction =
            Vector2f(0.0f, 0.5f) + 0.5f * m_lower_left_node->sample(sample, pdf);

        const float probability_factor = 4.0f * lower_left / m_previous_iter_radiance_sum;
        pdf *= probability_factor;
        return sampled_direction;
    }
    else
    {
        sample.x = (sample.x - factor) / (1.0f - factor);
        factor = upper_right / sum_right_half;

        if (sample.y < factor)
        {
            sample.y /= factor;
            const Vector2f sampled_direction =
                Vector2f(0.5f, 0.0f) + 0.5f * m_upper_right_node->sample(sample, pdf);

            const float probability_factor = 4.0f * upper_right / m_previous_iter_radiance_sum;
            pdf *= probability_factor;
            return sampled_direction;
        }

        sample.y = (sample.y - factor) / (1.0f - factor);
        const Vector2f sampled_direction =
            Vector2f(0.5f, 0.5f) + 0.5f * m_lower_right_node->sample(sample, pdf);

        const float probability_factor = 4.0f * lower_right / m_previous_iter_radiance_sum;
        pdf *= probability_factor;
        return sampled_direction;
    }
}

float QuadTreeNode::pdf(
    Vector2f&                           direction) const
{
    if(m_is_leaf)
        return RcpFourPi<float>();
    
    const QuadTreeNode* sub_node = choose_node(direction);
    const float factor = 4.0f * sub_node->m_previous_iter_radiance_sum / m_previous_iter_radiance_sum;
    return factor * sub_node->pdf(direction);
}

size_t QuadTreeNode::depth(
    Vector2f&                           direction) const
{
    if(m_is_leaf)
        return 1;
    
    return 1 + choose_node(direction)->depth(direction);
}

QuadTreeNode* QuadTreeNode::choose_node(
    Vector2f&                           direction) const
{
    if(direction.x < 0.5f)
    {
        direction.x *= 2.0f;
        if(direction.y < 0.5f)
        {
            direction.y *= 2.0f;
            return m_upper_left_node.get();
        }
        else
        {
            direction.y = direction.y * 2.0f - 1.0f;
            return m_lower_left_node.get();
        }
    }
    else
    {
        direction.x = direction.x * 2.0f - 1.0f;
        if(direction.y < 0.5f)
        {
            direction.y *= 2.0f;
            return m_upper_right_node.get();
        }
        else
        {
            direction.y = direction.y * 2.0f - 1.0f;
            return m_lower_right_node.get();
        }
    }
}

struct DTreeRecord
{
    Vector3f                    direction;
    float                       radiance;
    float                       wi_pdf;
    float                       bsdf_pdf;
    float                       d_tree_pdf;
    float                       sample_weight;
    float                       product;
    bool                        is_delta;
};

DTree::DTree(
    const GPTParameters&                parameters)
    : m_parameters(parameters)
    , m_root_node(true)
    , m_current_iter_sample_weight(0.0f)
    , m_previous_iter_sample_weight(0.0f)
    , m_optimization_step_count(0)
    , m_first_moment(0.0f)
    , m_second_moment(0.0f)
    , m_theta(0.0f)
    , m_atomic_flag(ATOMIC_FLAG_INIT)
    , m_is_built(false)
{}

DTree::DTree(
    const DTree&                        other)
    : m_parameters(other.m_parameters)
    , m_current_iter_sample_weight(other.m_current_iter_sample_weight.load(std::memory_order_relaxed))
    , m_previous_iter_sample_weight(other.m_previous_iter_sample_weight)
    , m_root_node(other.m_root_node)
    , m_optimization_step_count(other.m_optimization_step_count)
    , m_first_moment(other.m_first_moment)
    , m_second_moment(other.m_second_moment)
    , m_theta(other.m_theta)
    , m_atomic_flag(ATOMIC_FLAG_INIT)
    , m_is_built(other.m_is_built)
{}

void DTree::record(
    const DTreeRecord&                  d_tree_record)
{
    if(d_tree_record.is_delta || !std::isfinite(d_tree_record.sample_weight) || d_tree_record.sample_weight <= 0.0f)
        return;

    atomic_add(m_current_iter_sample_weight, d_tree_record.sample_weight);

    const float radiance = d_tree_record.radiance / d_tree_record.wi_pdf * d_tree_record.sample_weight;
    
    Vector2f direction = cartesian_to_cylindrical(d_tree_record.direction);

    switch (m_parameters.m_directional_filter)
    {
    case DirectionalFilter::Nearest:
        m_root_node.add_radiance(direction, radiance);
        break;

    case DirectionalFilter::Box:
    {
        const size_t leaf_depth = depth(direction);
        const Vector2f leaf_size(std::pow(0.5f, leaf_depth - 1));

        const AABB2f node_aabb(Vector2f(0.0f), Vector2f(1.0f));
        const AABB2f splat_aabb(direction - 0.5f * leaf_size, direction + 0.5f * leaf_size);

        if(!splat_aabb.is_valid())
            return;

        m_root_node.add_radiance(splat_aabb, node_aabb, radiance / splat_aabb.volume());
        break;
    }
    default:
        break;
    }

    if(m_parameters.m_bsdf_sampling_fraction_mode == BSDFSamplingFractionMode::Learn && m_is_built && d_tree_record.product > 0.0f)
        optimization_step(d_tree_record);
}

void DTree::sample(
    SamplingContext&                    sampling_context,
    DTreeSample&                        d_tree_sample) const
{
    sampling_context.split_in_place(2, 1);
    Vector2f s = sampling_context.next2<Vector2f>();

    if (m_previous_iter_sample_weight <= 0.0f || m_root_node.radiance_sum() <= 0.0f)
    {
        d_tree_sample.direction = sample_sphere_uniform(s);
        d_tree_sample.pdf = RcpFourPi<float>();
    }
    else
    {
        d_tree_sample.pdf = 1.0f;
        const Vector2f direction = m_root_node.sample(s, d_tree_sample.pdf);
        d_tree_sample.direction = cylindrical_to_cartesian(direction);
    }
}

float DTree::pdf(
    const Vector3f&                     direction) const
{
    if(m_previous_iter_sample_weight <= 0.0f || m_root_node.radiance_sum() <= 0.0f)
        return RcpFourPi<float>();

    Vector2f dir = cartesian_to_cylindrical(direction);
    return m_root_node.pdf(dir);
}

void DTree::halve_sample_weight()
{
    m_current_iter_sample_weight = 0.5f * m_current_iter_sample_weight.load(std::memory_order_relaxed);
    m_previous_iter_sample_weight *= 0.5f;
}

size_t DTree::node_count() const
{
    return m_root_node.node_count();
}

size_t DTree::max_depth() const
{
    return m_root_node.max_depth();
}

size_t DTree::depth(
    const Vector2f&                     direction) const
{
    Vector2f local_direction = direction;

    return m_root_node.depth(local_direction);
}

void DTree::build()
{
    m_previous_iter_sample_weight = m_current_iter_sample_weight.load(std::memory_order_relaxed);
    m_root_node.build_radiance_sums();
}

void DTree::restructure(
    const float                         subdiv_threshold)
{
    m_root_node.restructure(m_root_node.radiance_sum(), subdiv_threshold);
    m_current_iter_sample_weight.store(0.0f, std::memory_order_relaxed);
    m_is_built = true;
}

float DTree::sample_weight() const
{
    return m_previous_iter_sample_weight;
}

float DTree::mean() const
{
    if (m_previous_iter_sample_weight <= 0.0f)
        return 0.0f;

    return m_root_node.radiance_sum() * (1.0f / m_previous_iter_sample_weight) * RcpFourPi<float>();
}

float DTree::bsdf_sampling_fraction() const
{
    if(m_parameters.m_bsdf_sampling_fraction_mode == BSDFSamplingFractionMode::Learn)
        return logistic(m_theta);
    else
        return m_parameters.m_fixed_bsdf_sampling_fraction;
}

void DTree::acquire_optimization_spin_lock()
{
    while(m_atomic_flag.test_and_set(std::memory_order_acquire))
        ;
}

void DTree::release_optimization_spin_lock()
{
    m_atomic_flag.clear(std::memory_order_release);
}

// BSDF sampling fraction optimization procedure.
// Implementation of Algorithm 3 in chapter "Practical Path Guiding in Production" [Müller 2019]
// released in "Path Guiding in Production" Siggraph Course 2019, [Vorba et. al. 2019]

void DTree::adam_step(
    const float                         gradient)
{
    ++m_optimization_step_count;
    const float debiased_learning_rate = m_parameters.m_learning_rate *
                                         std::sqrt(1.0f - std::pow(Beta2, m_optimization_step_count)) /
                                         (1.0f - std::pow(Beta1, m_optimization_step_count));

    m_first_moment = Beta1 * m_first_moment + (1.0f - Beta1) * gradient;
    m_second_moment = Beta2 * m_second_moment + (1.0f - Beta2) * gradient * gradient;
    m_theta -= debiased_learning_rate * m_first_moment / (std::sqrt(m_second_moment) + OptimizationEpsilon);

    m_theta = clamp(m_theta, -20.0f, 20.0f);
}

void DTree::optimization_step(
    const DTreeRecord&                  d_tree_record)
{
    acquire_optimization_spin_lock();

    const float sampling_fraction = bsdf_sampling_fraction();
    const float combined_pdf = sampling_fraction * d_tree_record.bsdf_pdf +
                               (1.0f - sampling_fraction) * d_tree_record.d_tree_pdf;

    const float d_sampling_fraction = -d_tree_record.product *
                                      (d_tree_record.bsdf_pdf - d_tree_record.d_tree_pdf) /
                                      (d_tree_record.wi_pdf * combined_pdf);

    const float d_theta = d_sampling_fraction * sampling_fraction * (1.0f - sampling_fraction);
    const float reg_gradient = m_theta * Regularization;
    const float gradient = (d_theta + reg_gradient) * d_tree_record.sample_weight;

    adam_step(gradient);

    release_optimization_spin_lock();
}

struct DTreeStatistics
{
    DTreeStatistics()
      : max_d_tree_depth(0)
      , min_max_d_tree_depth(std::numeric_limits<size_t>::max())
      , average_max_d_tree_depth(0)
      , max_s_tree_depth(0)
      , min_max_s_tree_depth(std::numeric_limits<size_t>::max())
      , average_max_s_tree_depth(0)
      , max_mean_radiance(0)
      , min_mean_radiance(std::numeric_limits<float>::max())
      , average_mean_radiance(0)
      , max_d_tree_nodes(0)
      , min_d_tree_nodes(std::numeric_limits<size_t>::max())
      , average_d_tree_nodes(0)
      , max_sample_weight(0)
      , min_sample_weight(std::numeric_limits<float>::max())
      , average_sample_weight(0)
      , num_d_trees(0)
      , num_s_tree_nodes(0)
    {}

    size_t                              max_d_tree_depth;
    size_t                              min_max_d_tree_depth;
    float                               average_max_d_tree_depth;
    size_t                              max_s_tree_depth;
    size_t                              min_max_s_tree_depth;
    float                               average_max_s_tree_depth;
    float                               max_mean_radiance;
    float                               min_mean_radiance;
    float                               average_mean_radiance;
    size_t                              max_d_tree_nodes;
    size_t                              min_d_tree_nodes;
    float                               average_d_tree_nodes;
    float                               max_sample_weight;
    float                               min_sample_weight;
    float                               average_sample_weight;
    size_t                              num_d_trees;
    size_t                              num_s_tree_nodes;

    void build()
    {
        if(num_d_trees <= 0)
            return;

        average_max_d_tree_depth /= num_d_trees;
        average_max_s_tree_depth /= num_d_trees;
        average_d_tree_nodes /= num_d_trees;
        average_mean_radiance /= num_d_trees;
        average_sample_weight /= num_d_trees;
    }
};

STreeNode::STreeNode(
    const GPTParameters&                parameters)
    : m_axis(0)
    , m_d_tree(new DTree(parameters))
{}

STreeNode::STreeNode(
    const unsigned int                  parent_axis,
    const DTree*                        parent_d_tree)
    : m_axis((parent_axis + 1) % 3)
    , m_d_tree(new DTree(*parent_d_tree))
{
    m_d_tree->halve_sample_weight();
}

DTree* STreeNode::get_d_tree(
    Vector3f&                           point,
    Vector3f&                           size)
{
    if(is_leaf())
        return m_d_tree.get();
    else
    {
        size[m_axis] *= 0.5f;
        return choose_node(point)->get_d_tree(point, size);
    }
}

void STreeNode::subdivide(
    const size_t                        required_samples)
{
    if(is_leaf())
    {
        if (m_d_tree->sample_weight() > required_samples)
            subdivide();
        else
            return;
    }
    
    m_first_node->subdivide(required_samples);
    m_second_node->subdivide(required_samples);
}

void STreeNode::record(
    const AABB3f&                       splat_aabb,
    const AABB3f&                       node_aabb,
    const DTreeRecord&                  d_tree_record)
{
    const AABB3f intersection_aabb(AABB3f::intersect(splat_aabb, node_aabb));

    if(!intersection_aabb.is_valid())
        return;

    const float intersection_volume = intersection_aabb.volume();

    if(intersection_volume <= 0.0f)
        return;

    if(is_leaf())
    {
        m_d_tree->record(DTreeRecord{
                            d_tree_record.direction,
                            d_tree_record.radiance,
                            d_tree_record.wi_pdf,
                            d_tree_record.bsdf_pdf,
                            d_tree_record.d_tree_pdf,
                            d_tree_record.sample_weight * intersection_volume,
                            d_tree_record.product,
                            d_tree_record.is_delta});
    }
    else
    {
        const Vector3f node_size = node_aabb.extent();
        Vector3f offset(0.0f);
        offset[m_axis] = node_size[m_axis] * 0.5f;

        m_first_node->record(splat_aabb, AABB3f(node_aabb.min, node_aabb.max - offset), d_tree_record);
        m_second_node->record(splat_aabb, AABB3f(node_aabb.min + offset, node_aabb.max), d_tree_record);
    }
}

void STreeNode::restructure(
    const float                         subdiv_threshold)
{
    if(is_leaf())
    {
        m_d_tree->restructure(subdiv_threshold);
    }
    else
    {
        m_first_node->restructure(subdiv_threshold);
        m_second_node->restructure(subdiv_threshold);
    }
}

void STreeNode::build()
{
    if(is_leaf())
    {
        m_d_tree->build();
    }
    else
    {
        m_first_node->build();
        m_second_node->build();
    }
}

void STreeNode::gather_statistics(
    DTreeStatistics&                    statistics,
    const size_t                        depth) const
{
    statistics.num_s_tree_nodes++;
    if(is_leaf())
    {
        ++statistics.num_d_trees;
        const size_t d_tree_depth = m_d_tree->max_depth();
        statistics.max_d_tree_depth = std::max(statistics.max_d_tree_depth, d_tree_depth);
        statistics.min_max_d_tree_depth = std::min(statistics.min_max_d_tree_depth, d_tree_depth);
        statistics.average_max_d_tree_depth += d_tree_depth;
        statistics.max_s_tree_depth = std::max(statistics.max_s_tree_depth, depth);
        statistics.min_max_s_tree_depth = std::min(statistics.min_max_s_tree_depth, depth);
        statistics.average_max_s_tree_depth += depth;

        const float mean_radiance = m_d_tree->mean();
        statistics.max_mean_radiance = std::max(statistics.max_mean_radiance, mean_radiance);
        statistics.min_mean_radiance = std::min(statistics.max_mean_radiance, mean_radiance);
        statistics.average_mean_radiance += mean_radiance;

        const size_t node_count = m_d_tree->node_count();
        statistics.max_d_tree_nodes = std::max(statistics.max_d_tree_nodes, node_count);
        statistics.min_d_tree_nodes = std::min(statistics.min_d_tree_nodes, node_count);
        statistics.average_d_tree_nodes += node_count;

        const float sample_weight = m_d_tree->sample_weight();
        statistics.max_sample_weight = std::max(statistics.max_sample_weight, sample_weight);
        statistics.min_sample_weight = std::min(statistics.min_sample_weight, sample_weight);
        statistics.average_sample_weight += sample_weight;
    }
    else
    {
        m_first_node->gather_statistics(statistics, depth + 1);
        m_second_node->gather_statistics(statistics, depth + 1);
    }
}

STreeNode* STreeNode::choose_node(
    Vector3f&                           point) const
{
    if(point[m_axis] < 0.5f)
    {
        point[m_axis] *= 2.0f;
        return m_first_node.get();
    }
    else
    {
        point[m_axis] = (point[m_axis] - 0.5f) * 2.0f;
        return m_second_node.get();
    }
}

void STreeNode::subdivide()
{
    if(is_leaf())
    {
        m_first_node.reset(new STreeNode(m_axis, m_d_tree.get()));
        m_second_node.reset(new STreeNode(m_axis, m_d_tree.get()));
        m_d_tree.reset(nullptr);
    }
}

bool STreeNode::is_leaf() const
{
    return m_d_tree != nullptr;
}

STree::STree(
    const AABB3f&                       scene_aabb,
    const GPTParameters&                parameters)
    : m_parameters(parameters)
    , m_scene_aabb(scene_aabb)
    , m_is_built(false)
    , m_is_final_iteration(false)
{
    m_root_node.reset(new STreeNode(m_parameters));

    // Grow the AABB into a cube for nicer hierarchical subdivisions [Müller et. al. 2017].
    const Vector3f size = m_scene_aabb.extent();
    const float maxSize = max_value(size);
    m_scene_aabb.max = m_scene_aabb.min + Vector3f(maxSize);
}

DTree* STree::get_d_tree(
    const Vector3f&                     point,
    Vector3f&                           d_tree_voxel_size)
{
    d_tree_voxel_size = m_scene_aabb.extent();
    Vector3f transformed_point = point - m_scene_aabb.min;
    transformed_point /= d_tree_voxel_size;

    return m_root_node->get_d_tree(transformed_point, d_tree_voxel_size);
}

DTree* STree::get_d_tree(
    const Vector3f&                     point)
{
    Vector3f d_tree_voxel_size;
    return get_d_tree(point, d_tree_voxel_size);
}

void STree::record(
    DTree*                              d_tree,
    const Vector3f&                     point,
    const Vector3f&                     d_tree_node_size,
    DTreeRecord                         d_tree_record,
    SamplingContext&                    sampling_context)
{
    switch (m_parameters.m_spatial_filter)
    {
    case SpatialFilter::Nearest:
        d_tree->record(d_tree_record);
        break;

    case SpatialFilter::Stochastic:
    {
        sampling_context.split_in_place(3, 1);

        // Jitter the position of the record.
        Vector3f offset = d_tree_node_size;
        offset *= (sampling_context.next2<Vector3f>() - Vector3f(0.5f));
        Vector3f jittered_point = clip_vector_to_aabb(point + offset);

        DTree* stochastic_d_tree = get_d_tree(jittered_point);
        stochastic_d_tree->record(d_tree_record);
        break;
    }

    case SpatialFilter::Box:
        box_filter_splat(point, d_tree_node_size, d_tree_record);
        break;
    }
}

const AABB3f& STree::aabb() const
{
    return m_scene_aabb;
}

void STree::build(
    const size_t                        iteration)
{
    m_root_node->build();

    const size_t required_samples = std::sqrt(std::pow(2, iteration) * m_parameters.m_samples_per_pass * 0.25f) * SpatialSubdivisionThreshold;
    m_root_node->subdivide(required_samples);
    m_root_node->restructure(DTreeThreshold);

    DTreeStatistics statistics;
    m_root_node->gather_statistics(statistics);
    statistics.build();

    RENDERER_LOG_INFO(
        "SD tree statistics: [min, max, avg]\n"
        "  DTree Depth     = [%s, %s, %s]\n"
        "  STree Depth     = [%s, %s, %s]\n"
        "  Mean radiance   = [%s, %s, %s]\n"
        "  Node count      = [%s, %s, %s]\n"
        "  Sample weight   = [%s, %s, %s]\n\n",
        pretty_uint(statistics.min_max_d_tree_depth).c_str(), pretty_uint(statistics.max_d_tree_depth).c_str(), pretty_scalar(statistics.average_max_d_tree_depth, 2).c_str(),
        pretty_uint(statistics.min_max_s_tree_depth).c_str(), pretty_uint(statistics.max_s_tree_depth).c_str(), pretty_scalar(statistics.average_max_s_tree_depth, 2).c_str(),
        pretty_scalar(statistics.min_mean_radiance, 4).c_str(), pretty_scalar(statistics.max_mean_radiance, 4).c_str(), pretty_scalar(statistics.average_mean_radiance, 4).c_str(),
        pretty_uint(statistics.min_d_tree_nodes).c_str(), pretty_uint(statistics.max_d_tree_nodes).c_str(), pretty_scalar(statistics.average_d_tree_nodes, 4).c_str(),
        pretty_scalar(statistics.min_sample_weight, 4).c_str(), pretty_scalar(statistics.max_sample_weight, 4).c_str(), pretty_scalar(statistics.average_sample_weight, 4).c_str());

    m_is_built = true;
}

bool STree::is_built() const
{
    return m_is_built;
}

void STree::start_final_iteration()
{
    m_is_final_iteration = true;
}

bool STree::is_final_iteration() const
{
    return m_is_final_iteration;
}

void STree::box_filter_splat(
    const Vector3f&                     point,
    const Vector3f&                     d_tree_node_size,
    DTreeRecord&                        d_tree_record)
{
    const AABB3f splat_aabb(point - d_tree_node_size * 0.5f, point + d_tree_node_size * 0.5f);

    assert(splat_aabb.is_valid());

    d_tree_record.sample_weight /= splat_aabb.volume();
    m_root_node->record(AABB3f(point - d_tree_node_size * 0.5f, point + d_tree_node_size * 0.5f), m_scene_aabb, d_tree_record);
}

/// Clip a point to lie within bounding box.
Vector3f STree::clip_vector_to_aabb(
    const Vector3f&                     point)
{
    Vector3f result = point;
    for (int i = 0; i < Vector3f::Dimension; ++i)
    {
        result[i] = std::min(std::max(result[i], m_scene_aabb.min[i]), m_scene_aabb.max[i]);
    }
    return result;
}

void GPTVertex::add_radiance(
    const renderer::Spectrum&           radiance)
{
    m_radiance += radiance;
}

bool isValidSpectrum(
    const Spectrum&                     s)
{
    for (int i = 0; i < s.size(); i++)
        if (!std::isfinite(s[i]) || s[i] < 0.0f)
            return false;
    return true;
}

void GPTVertex::record_to_tree(
    STree&                              sd_tree,
    float                               statistical_weight,
    SamplingContext&                    sampling_context)
{
    if (!(m_wi_pdf > 0) || !isValidSpectrum(m_radiance) || !isValidSpectrum(m_bsdf_value))
    {
        return;
    }

    Spectrum incoming_radiance(0.0f);

    for(size_t i = 0; i < incoming_radiance.size(); ++i)
    {
        if(m_throughput[i] * m_wi_pdf > SDTreeEpsilon)
            incoming_radiance[i] = m_radiance[i] / m_throughput[i];
    }

    Spectrum product = incoming_radiance * m_bsdf_value;

    DTreeRecord d_tree_record{m_direction,
                             average_value(incoming_radiance),
                             m_wi_pdf,
                             m_bsdf_pdf,
                             m_d_tree_pdf,
                             statistical_weight,
                             average_value(product),
                             m_is_delta};

    sd_tree.record(m_d_tree, m_point, m_d_tree_node_size, d_tree_record, sampling_context);
}

GPTVertexPath::GPTVertexPath()
  : m_path_index(0)
{
}

void GPTVertexPath::add_vertex(
    const GPTVertex&                    vertex)
{
    if(m_path_index < m_path.size())
        m_path[m_path_index++] = vertex;
}

void GPTVertexPath::add_radiance(
    const renderer::Spectrum&           r)
{
    for(int i = 0; i < m_path_index; ++i)
        m_path[i].add_radiance(r);
}

bool GPTVertexPath::is_full() const
{
    return m_path_index >= m_path.size();
}

void GPTVertexPath::record_to_tree(
    STree&                              sd_tree,
    float                               statistical_weight,
    SamplingContext&                    sampling_context)
{
    for(int i = 0; i < m_path_index; ++i)
        m_path[i].record_to_tree(sd_tree,
                            statistical_weight,
                            sampling_context);
}

void GPTVertexPath::set_sampling_fraction(
    const float                         sampling_fraction)
{
    m_sampling_fraction = sampling_fraction;
}

float GPTVertexPath::get_sampling_fraction() const
{
    return m_sampling_fraction;
}

}   // namespace renderer
