#pragma once
// Pose helpers for world-locked panels (the VR menu, the game's menu screen).
#include "../vrmath.h"
#include "xr_system.h"

// A level pose `distance` metres in front of where the head looks (yaw only),
// `drop` metres below eye height, facing the player. LOCAL space.
inline XrPosef PoseInFrontOfHead(const XrFrame& frame, float distance, float drop)
{
    const XrPosef& eye = frame.views[0].pose;
    Vec3 head{(frame.views[0].pose.position.x + frame.views[1].pose.position.x) * 0.5f,
              (frame.views[0].pose.position.y + frame.views[1].pose.position.y) * 0.5f,
              (frame.views[0].pose.position.z + frame.views[1].pose.position.z) * 0.5f};
    Vec3 fwd = Rotate({eye.orientation.x, eye.orientation.y, eye.orientation.z, eye.orientation.w}, {0, 0, -1});
    float yaw = std::atan2(-fwd.x, -fwd.z);
    Quat q = YawQuat(yaw);
    Vec3 pos = head + Rotate(q, {0, -drop, -distance});
    XrPosef pose;
    pose.orientation = {q.x, q.y, q.z, q.w};
    pose.position = {pos.x, pos.y, pos.z};
    return pose;
}

// Where a ray (an aim pose's -Z) hits a quad layer of width_m x height_m at
// quad_pose (facing +Z). u, v are 0..1 from the top-left; false on a miss.
inline bool RayHitQuad(const XrPosef& aim, const XrPosef& quad_pose, float width_m, float height_m, float& u,
                       float& v)
{
    Quat qa{aim.orientation.x, aim.orientation.y, aim.orientation.z, aim.orientation.w};
    Vec3 o{aim.position.x, aim.position.y, aim.position.z};
    Vec3 d = Rotate(qa, {0, 0, -1});

    Quat qp{quad_pose.orientation.x, quad_pose.orientation.y, quad_pose.orientation.z, quad_pose.orientation.w};
    Vec3 center{quad_pose.position.x, quad_pose.position.y, quad_pose.position.z};
    Vec3 n = Rotate(qp, {0, 0, 1});
    float denom = Dot(d, n);
    if (denom > -1e-4f)
        return false;
    float t = Dot(center - o, n) / denom;
    if (t < 0)
        return false;
    Vec3 local = Rotate(Conjugate(qp), (o + d * t) - center);
    u = local.x / width_m + 0.5f;
    v = 0.5f - local.y / height_m;
    return u >= 0 && v >= 0 && u <= 1 && v <= 1;
}
