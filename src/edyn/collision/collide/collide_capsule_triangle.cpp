#include "edyn/collision/collide.hpp"
#include "edyn/util/shape_util.hpp"
#include "edyn/math/math.hpp"

namespace edyn {

void collide(const capsule_shape &capsule, const triangle_mesh &mesh, size_t tri_idx,
             const collision_context &ctx, collision_result &result) {
    const auto &posA = ctx.posA;
    const auto &ornA = ctx.ornA;

    const auto capsule_vertices = capsule.get_vertices(posA, ornA);

    const auto tri_vertices = mesh.get_triangle_vertices(tri_idx);
    const auto tri_normal = mesh.get_triangle_normal(tri_idx);

    triangle_feature tri_feature;
    size_t tri_feature_index;
    auto projection_cap = -EDYN_SCALAR_MAX;
    auto projection_tri = -EDYN_SCALAR_MAX;
    auto sep_axis = vector3_zero;
    auto distance = -EDYN_SCALAR_MAX;

    // Triangle face normal.
    {
        auto dir = tri_normal;
        auto proj_cap = -capsule_support_projection(capsule_vertices, capsule.radius, -dir);
        auto proj_tri = dot(tri_vertices[0], tri_normal);
        auto dist = proj_cap - proj_tri;

        if (dist > distance) {
            distance = dist;
            projection_cap = proj_cap;
            projection_tri = proj_tri;
            tri_feature = triangle_feature::face;
            sep_axis = tri_normal;
        }
    }

    // Triangle edges vs capsule edge.
    for (size_t i = 0; i < 3; ++i) {
        auto &v0 = tri_vertices[i];
        auto &v1 = tri_vertices[(i + 1) % 3];
        scalar s, t;
        vector3 closest_tri, closest_cap;
        closest_point_segment_segment(capsule_vertices[0], capsule_vertices[1],
                                      v0, v1, s, t, closest_cap, closest_tri);

        auto dir = closest_tri - closest_cap;

        if (!try_normalize(dir)) {
            // Segments intersect in 3D space (unlikely scenario). Try the cross
            // product between edges.
            auto tri_edge = v1 - v0;
            dir = cross(tri_edge, capsule_vertices[1] - capsule_vertices[0]);

            if (!try_normalize(dir)) {
                // Segments are parallel and colinear.
                continue;
            }
        }

        if (dot(posA - v0, dir) < 0) {
            dir *= -1; // Make it point towards capsule.
        }

        triangle_feature feature;
        size_t feature_idx;
        scalar proj_tri;
        get_triangle_support_feature(tri_vertices, vector3_zero, dir, feature,
                                     feature_idx, proj_tri, support_feature_tolerance);

        if (mesh.ignore_triangle_feature(tri_idx, feature, feature_idx, dir)) {
            continue;
        }

        auto proj_cap = -capsule_support_projection(capsule_vertices, capsule.radius, -dir);
        auto dist = proj_cap - proj_tri;

        if (dist > distance) {
            distance = dist;
            projection_cap = proj_cap;
            projection_tri = proj_tri;
            tri_feature = feature;
            tri_feature_index = feature_idx;
            sep_axis = dir;
        }
    }

    if (distance > ctx.threshold) {
        return;
    }

    scalar proj_capsule_vertices[] = {
        dot(capsule_vertices[0], sep_axis),
        dot(capsule_vertices[1], sep_axis)
    };

    auto is_capsule_edge = std::abs(proj_capsule_vertices[0] -
                                    proj_capsule_vertices[1]) < support_feature_tolerance;

    auto capsule_vertex_index = proj_capsule_vertices[0] < proj_capsule_vertices[1] ? 0 : 1;

    switch (tri_feature) {
    case triangle_feature::face: {
        if (is_capsule_edge) {
            // Check if capsule vertex is inside triangle face.
            for (auto &vertex : capsule_vertices) {
                if (point_in_triangle(tri_vertices, sep_axis, vertex)) {
                    auto pivotA_world = vertex - sep_axis * capsule.radius;
                    auto pivotA = to_object_space(pivotA_world, posA, ornA);
                    auto pivotB = project_plane(vertex, tri_vertices[0], sep_axis);
                    auto local_distance = dot(pivotA_world - tri_vertices[0], sep_axis);
                    result.maybe_add_point({pivotA, pivotB, sep_axis, local_distance});
                }
            }

            // Both vertices are inside the triangle. Unnecessary to look for intersections.
            if (result.num_points == 2) {
                return;
            }

            // Check if the capsule edge intersects the triangle edges.
            auto &tri_origin = tri_vertices[0];
            auto tangent = normalize(tri_vertices[1] - tri_vertices[0]);
            auto bitangent = cross(tri_normal, tangent);
            auto tri_basis = matrix3x3_columns(tangent, tri_normal, bitangent);

            auto p0 = to_vector2_xz(to_object_space(capsule_vertices[0], tri_origin, tri_basis));
            auto p1 = to_vector2_xz(to_object_space(capsule_vertices[1], tri_origin, tri_basis));

            for (int i = 0; i < 3; ++i) {
                // Ignore concave edges.
                if (mesh.is_concave_edge(mesh.get_face_edge_index(tri_idx, i))) {
                    continue;
                }

                auto &v0 = tri_vertices[i];
                auto &v1 = tri_vertices[(i + 1) % 3];
                auto q0 = to_vector2_xz(to_object_space(v0, tri_origin, tri_basis));
                auto q1 = to_vector2_xz(to_object_space(v1, tri_origin, tri_basis));

                scalar s[2], t[2];
                auto num_points = intersect_segments(p0, p1, q0, q1, s[0], t[0], s[1], t[1]);

                for (size_t k = 0; k < num_points; ++k) {
                    auto pivotA_world = lerp(capsule_vertices[0], capsule_vertices[1], s[k]) - sep_axis * capsule.radius;
                    auto pivotA = to_object_space(pivotA_world, posA, ornA);
                    auto pivotB = lerp(v0, v1, t[k]);
                    auto local_distance = dot(pivotA_world - tri_vertices[0], sep_axis);
                    result.maybe_add_point({pivotA, pivotB, sep_axis, local_distance});
                }
            }
        } else {
            // Triangle face against capsule vertex.
            auto &closest_capsule_vertex = capsule_vertices[capsule_vertex_index];

            if (point_in_triangle(tri_vertices, tri_normal, closest_capsule_vertex)) {
                auto pivotA_world = closest_capsule_vertex - sep_axis * capsule.radius;
                auto pivotA = to_object_space(pivotA_world, posA, ornA);
                auto pivotB = project_plane(closest_capsule_vertex, tri_vertices[0], sep_axis);
                result.maybe_add_point({pivotA, pivotB, sep_axis, distance});
            }
        }
        break;
    }
    case triangle_feature::edge: {
        auto &v0 = tri_vertices[tri_feature_index];
        auto &v1 = tri_vertices[(tri_feature_index + 1) % 3];

        if (is_capsule_edge) {
            scalar s[2], t[2];
            vector3 closest_tri[2], closest_cap[2];
            size_t num_points;
            closest_point_segment_segment(capsule_vertices[0], capsule_vertices[1], v0, v1,
                                          s[0], t[0], closest_cap[0], closest_tri[0], &num_points,
                                          &s[1], &t[1], &closest_cap[1], &closest_tri[1]);

            for (size_t i = 0; i < num_points; ++i) {
                auto pivotA_world = closest_cap[i] - sep_axis * capsule.radius;
                auto pivotA = to_object_space(pivotA_world, posA, ornA);
                auto pivotB = closest_tri[i];
                result.maybe_add_point({pivotA, pivotB, sep_axis, distance});
            }
        } else {
            auto &closest_capsule_vertex = capsule_vertices[capsule_vertex_index];
            vector3 pivotB; scalar t;
            closest_point_line(v0, v1 - v0, closest_capsule_vertex, t, pivotB);

            auto pivotA_world = closest_capsule_vertex - sep_axis * capsule.radius;
            auto pivotA = to_object_space(pivotA_world, posA, ornA);
            result.maybe_add_point({pivotA, pivotB, sep_axis, distance});
        }
        break;
    }
    case triangle_feature::vertex: {
        auto &pivotB = tri_vertices[tri_feature_index];

        if (is_capsule_edge) {
            auto edge = capsule_vertices[1] - capsule_vertices[0];
            vector3 closest; scalar t;
            closest_point_line(capsule_vertices[0], edge, pivotB, t, closest);

            auto pivotA_world = closest - sep_axis * capsule.radius;
            auto pivotA = to_object_space(pivotA_world, posA, ornA);
            result.maybe_add_point({pivotA, pivotB, sep_axis, distance});
        } else {
            auto &closest_capsule_vertex = capsule_vertices[capsule_vertex_index];
            auto pivotA_world = closest_capsule_vertex - sep_axis * capsule.radius;
            auto pivotA = to_object_space(pivotA_world, posA, ornA);
            result.maybe_add_point({pivotA, pivotB, sep_axis, distance});
        }
    }
    }
}

}
