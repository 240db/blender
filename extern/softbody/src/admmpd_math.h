// Copyright Matt Overby 2020.
// Distributed under the MIT License.

#ifndef ADMMPD_MATH_H_
#define ADMMPD_MATH_H_

#include <Eigen/Geometry>

namespace admmpd {
namespace barycoords {

Eigen::Vector4d point_tet(
    const Eigen::Vector3d &p,
    const Eigen::Vector3d &a,
    const Eigen::Vector3d &b,
    const Eigen::Vector3d &c,
    const Eigen::Vector3d &d);

} // namespace barycoords

bool point_in_tet(
    const Eigen::Vector3d &p,
    const Eigen::Vector3d &a,
    const Eigen::Vector3d &b,
    const Eigen::Vector3d &c,
    const Eigen::Vector3d &d);

} // namespace admmpd

#endif // ADMMPD_MATH_H_
