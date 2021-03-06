#include "libslic3r/libslic3r.h"
#include "Selection.hpp"

#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectManipulation.hpp"
#include "GUI_ObjectList.hpp"
#include "Gizmos/GLGizmoBase.hpp"

#include <GL/glew.h>

#include <boost/algorithm/string/predicate.hpp>

static const float UNIFORM_SCALE_COLOR[3] = { 1.0f, 0.38f, 0.0f };

namespace Slic3r {
namespace GUI {

Selection::VolumeCache::TransformCache::TransformCache()
    : position(Vec3d::Zero())
    , rotation(Vec3d::Zero())
    , scaling_factor(Vec3d::Ones())
    , mirror(Vec3d::Ones())
    , rotation_matrix(Transform3d::Identity())
    , scale_matrix(Transform3d::Identity())
    , mirror_matrix(Transform3d::Identity())
    , full_matrix(Transform3d::Identity())
{
}

Selection::VolumeCache::TransformCache::TransformCache(const Geometry::Transformation& transform)
    : position(transform.get_offset())
    , rotation(transform.get_rotation())
    , scaling_factor(transform.get_scaling_factor())
    , mirror(transform.get_mirror())
    , full_matrix(transform.get_matrix())
{
    rotation_matrix = Geometry::assemble_transform(Vec3d::Zero(), rotation);
    scale_matrix = Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), scaling_factor);
    mirror_matrix = Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), Vec3d::Ones(), mirror);
}

Selection::VolumeCache::VolumeCache(const Geometry::Transformation& volume_transform, const Geometry::Transformation& instance_transform)
    : m_volume(volume_transform)
    , m_instance(instance_transform)
{
}

Selection::Selection()
    : m_volumes(nullptr)
    , m_model(nullptr)
    , m_mode(Instance)
    , m_type(Empty)
    , m_valid(false)
    , m_bounding_box_dirty(true)
    , m_curved_arrow(16)
    , m_scale_factor(1.0f)
{
#if ENABLE_RENDER_SELECTION_CENTER
    m_quadric = ::gluNewQuadric();
    if (m_quadric != nullptr)
        ::gluQuadricDrawStyle(m_quadric, GLU_FILL);
#endif // ENABLE_RENDER_SELECTION_CENTER
}

#if ENABLE_RENDER_SELECTION_CENTER
Selection::~Selection()
{
    if (m_quadric != nullptr)
        ::gluDeleteQuadric(m_quadric);
}
#endif // ENABLE_RENDER_SELECTION_CENTER

void Selection::set_volumes(GLVolumePtrs* volumes)
{
    m_volumes = volumes;
    _update_valid();
}

bool Selection::init(bool useVBOs)
{
    if (!m_arrow.init(useVBOs))
        return false;

    m_arrow.set_scale(5.0 * Vec3d::Ones());

    if (!m_curved_arrow.init(useVBOs))
        return false;

    m_curved_arrow.set_scale(5.0 * Vec3d::Ones());
    return true;
}

void Selection::set_model(Model* model)
{
    m_model = model;
    _update_valid();
}

void Selection::add(unsigned int volume_idx, bool as_single_selection)
{
    if (!m_valid || ((unsigned int)m_volumes->size() <= volume_idx))
        return;

    const GLVolume* volume = (*m_volumes)[volume_idx];
    // wipe tower is already selected
    if (is_wipe_tower() && volume->is_wipe_tower)
        return;

    // resets the current list if needed
    bool needs_reset = as_single_selection;
    needs_reset |= volume->is_wipe_tower;
    needs_reset |= is_wipe_tower() && !volume->is_wipe_tower;
    needs_reset |= !is_modifier() && volume->is_modifier;
    needs_reset |= is_modifier() && !volume->is_modifier;

    if (needs_reset)
        clear();

    if (volume->is_modifier)
        m_mode = Volume;
    else if (!contains_volume(volume_idx))
        m_mode = Instance;
    // else -> keep current mode

    switch (m_mode)
    {
    case Volume:
    {
        if (volume->volume_idx() >= 0 && (is_empty() || (volume->instance_idx() == get_instance_idx())))
            _add_volume(volume_idx);

        break;
    }
    case Instance:
    {
        _add_instance(volume->object_idx(), volume->instance_idx());
        break;
    }
    }

    _update_type();
    m_bounding_box_dirty = true;
}

void Selection::remove(unsigned int volume_idx)
{
    if (!m_valid || ((unsigned int)m_volumes->size() <= volume_idx))
        return;

    GLVolume* volume = (*m_volumes)[volume_idx];

    switch (m_mode)
    {
    case Volume:
    {
        _remove_volume(volume_idx);
        break;
    }
    case Instance:
    {
        _remove_instance(volume->object_idx(), volume->instance_idx());
        break;
    }
    }

    _update_type();
    m_bounding_box_dirty = true;
}

void Selection::add_object(unsigned int object_idx, bool as_single_selection)
{
    if (!m_valid)
        return;

    // resets the current list if needed
    if (as_single_selection)
        clear();

    m_mode = Instance;

    _add_object(object_idx);

    _update_type();
    m_bounding_box_dirty = true;
}

void Selection::remove_object(unsigned int object_idx)
{
    if (!m_valid)
        return;

    _remove_object(object_idx);

    _update_type();
    m_bounding_box_dirty = true;
}

void Selection::add_instance(unsigned int object_idx, unsigned int instance_idx, bool as_single_selection)
{
    if (!m_valid)
        return;

    // resets the current list if needed
    if (as_single_selection)
        clear();

    m_mode = Instance;

    _add_instance(object_idx, instance_idx);

    _update_type();
    m_bounding_box_dirty = true;
}

void Selection::remove_instance(unsigned int object_idx, unsigned int instance_idx)
{
    if (!m_valid)
        return;

    _remove_instance(object_idx, instance_idx);

    _update_type();
    m_bounding_box_dirty = true;
}

void Selection::add_volume(unsigned int object_idx, unsigned int volume_idx, int instance_idx, bool as_single_selection)
{
    if (!m_valid)
        return;

    // resets the current list if needed
    if (as_single_selection)
        clear();

    m_mode = Volume;

    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i)
    {
        GLVolume* v = (*m_volumes)[i];
        if ((v->object_idx() == object_idx) && (v->volume_idx() == volume_idx))
        {
            if ((instance_idx != -1) && (v->instance_idx() == instance_idx))
                _add_volume(i);
        }
    }

    _update_type();
    m_bounding_box_dirty = true;
}

void Selection::remove_volume(unsigned int object_idx, unsigned int volume_idx)
{
    if (!m_valid)
        return;

    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i)
    {
        GLVolume* v = (*m_volumes)[i];
        if ((v->object_idx() == object_idx) && (v->volume_idx() == volume_idx))
            _remove_volume(i);
    }

    _update_type();
    m_bounding_box_dirty = true;
}

void Selection::add_all()
{
    if (!m_valid)
        return;

    m_mode = Instance;
    clear();

    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i)
    {
        if (!(*m_volumes)[i]->is_wipe_tower)
            _add_volume(i);
    }

    _update_type();
    m_bounding_box_dirty = true;
}

void Selection::clear()
{
    if (!m_valid)
        return;

    for (unsigned int i : m_list)
    {
        (*m_volumes)[i]->selected = false;
    }

    m_list.clear();

    _update_type();
    m_bounding_box_dirty = true;

    // resets the cache in the sidebar
    wxGetApp().obj_manipul()->reset_cache();
}

// Update the selection based on the map from old indices to new indices after m_volumes changed.
// If the current selection is by instance, this call may select newly added volumes, if they belong to already selected instances.
void Selection::volumes_changed(const std::vector<size_t> &map_volume_old_to_new)
{
    assert(m_valid);

    // 1) Update the selection set.
    IndicesList list_new;
    std::vector<std::pair<unsigned int, unsigned int>> model_instances;
    for (unsigned int idx : m_list) {
        if (map_volume_old_to_new[idx] != size_t(-1)) {
            unsigned int new_idx = (unsigned int)map_volume_old_to_new[idx];
            list_new.insert(new_idx);
            if (m_mode == Instance) {
                // Save the object_idx / instance_idx pair of selected old volumes,
                // so we may add the newly added volumes of the same object_idx / instance_idx pair
                // to the selection.
                const GLVolume *volume = (*m_volumes)[new_idx];
                model_instances.emplace_back(volume->object_idx(), volume->instance_idx());
            }
        }
    }
    m_list = std::move(list_new);

    if (!model_instances.empty()) {
        // Instance selection mode. Add the newly added volumes of the same object_idx / instance_idx pair
        // to the selection.
        assert(m_mode == Instance);
        sort_remove_duplicates(model_instances);
        for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i) {
            const GLVolume* volume = (*m_volumes)[i];
            for (const std::pair<int, int> &model_instance : model_instances)
                if (volume->object_idx() == model_instance.first && volume->instance_idx() == model_instance.second)
                    this->_add_volume(i);
        }
    }

    _update_type();
    m_bounding_box_dirty = true;
}

bool Selection::is_single_full_instance() const
{
    if (m_type == SingleFullInstance)
        return true;

    if (m_type == SingleFullObject)
        return get_instance_idx() != -1;

    if (m_list.empty() || m_volumes->empty())
        return false;

    int object_idx = m_valid ? get_object_idx() : -1;
    if ((object_idx < 0) || ((int)m_model->objects.size() <= object_idx))
        return false;

    int instance_idx = (*m_volumes)[*m_list.begin()]->instance_idx();

    std::set<int> volumes_idxs;
    for (unsigned int i : m_list)
    {
        const GLVolume* v = (*m_volumes)[i];
        if ((object_idx != v->object_idx()) || (instance_idx != v->instance_idx()))
            return false;

        int volume_idx = v->volume_idx();
        if (volume_idx >= 0)
            volumes_idxs.insert(volume_idx);
    }

    return m_model->objects[object_idx]->volumes.size() == volumes_idxs.size();
}

bool Selection::is_from_single_object() const
{
    int idx = get_object_idx();
    return (0 <= idx) && (idx < 1000);
}

bool Selection::requires_uniform_scale() const
{
    if (is_single_full_instance() || is_single_modifier() || is_single_volume())
        return false;

    return true;
}

int Selection::get_object_idx() const
{
    return (m_cache.content.size() == 1) ? m_cache.content.begin()->first : -1;
}

int Selection::get_instance_idx() const
{
    if (m_cache.content.size() == 1)
    {
        const InstanceIdxsList& idxs = m_cache.content.begin()->second;
        if (idxs.size() == 1)
            return *idxs.begin();
    }

    return -1;
}

const Selection::InstanceIdxsList& Selection::get_instance_idxs() const
{
    assert(m_cache.content.size() == 1);
    return m_cache.content.begin()->second;
}

const GLVolume* Selection::get_volume(unsigned int volume_idx) const
{
    return (m_valid && (volume_idx < (unsigned int)m_volumes->size())) ? (*m_volumes)[volume_idx] : nullptr;
}

const BoundingBoxf3& Selection::get_bounding_box() const
{
    if (m_bounding_box_dirty)
        _calc_bounding_box();

    return m_bounding_box;
}

void Selection::start_dragging()
{
    if (!m_valid)
        return;

    _set_caches();
}

void Selection::translate(const Vec3d& displacement, bool local)
{
    if (!m_valid)
        return;

    for (unsigned int i : m_list)
    {
        if ((m_mode == Volume) || (*m_volumes)[i]->is_wipe_tower)
        {
            if (local)
                (*m_volumes)[i]->set_volume_offset(m_cache.volumes_data[i].get_volume_position() + displacement);
            else
            {
                Vec3d local_displacement = (m_cache.volumes_data[i].get_instance_rotation_matrix() * m_cache.volumes_data[i].get_instance_scale_matrix() * m_cache.volumes_data[i].get_instance_mirror_matrix()).inverse() * displacement;
                (*m_volumes)[i]->set_volume_offset(m_cache.volumes_data[i].get_volume_position() + local_displacement);
            }
        }
        else if (m_mode == Instance)
            (*m_volumes)[i]->set_instance_offset(m_cache.volumes_data[i].get_instance_position() + displacement);
    }

#if !DISABLE_INSTANCES_SYNCH
    if (m_mode == Instance)
        _synchronize_unselected_instances(SYNC_ROTATION_NONE);
    else if (m_mode == Volume)
        _synchronize_unselected_volumes();
#endif // !DISABLE_INSTANCES_SYNCH

    m_bounding_box_dirty = true;
}

static Eigen::Quaterniond rotation_xyz_diff(const Vec3d &rot_xyz_from, const Vec3d &rot_xyz_to)
{
    return
        // From the current coordinate system to world.
        Eigen::AngleAxisd(rot_xyz_to(2), Vec3d::UnitZ()) * Eigen::AngleAxisd(rot_xyz_to(1), Vec3d::UnitY()) * Eigen::AngleAxisd(rot_xyz_to(0), Vec3d::UnitX()) *
        // From world to the initial coordinate system.
        Eigen::AngleAxisd(-rot_xyz_from(0), Vec3d::UnitX()) * Eigen::AngleAxisd(-rot_xyz_from(1), Vec3d::UnitY()) * Eigen::AngleAxisd(-rot_xyz_from(2), Vec3d::UnitZ());
}

// This should only be called if it is known, that the two rotations only differ in rotation around the Z axis.
static double rotation_diff_z(const Vec3d &rot_xyz_from, const Vec3d &rot_xyz_to)
{
    Eigen::AngleAxisd angle_axis(rotation_xyz_diff(rot_xyz_from, rot_xyz_to));
    Vec3d  axis = angle_axis.axis();
    double angle = angle_axis.angle();
#ifndef NDEBUG
    if (std::abs(angle) > 1e-8) {
        assert(std::abs(axis.x()) < 1e-8);
        assert(std::abs(axis.y()) < 1e-8);
    }
#endif /* NDEBUG */
    return (axis.z() < 0) ? -angle : angle;
}

// Rotate an object around one of the axes. Only one rotation component is expected to be changing.
void Selection::rotate(const Vec3d& rotation, TransformationType transformation_type)
{
    if (!m_valid)
        return;

    // Only relative rotation values are allowed in the world coordinate system.
    assert(!transformation_type.world() || transformation_type.relative());

    int rot_axis_max = 0;
    if (rotation.isApprox(Vec3d::Zero()))
    {
        for (unsigned int i : m_list)
        {
            GLVolume &volume = *(*m_volumes)[i];
            if (m_mode == Instance)
            {
                volume.set_instance_rotation(m_cache.volumes_data[i].get_instance_rotation());
                volume.set_instance_offset(m_cache.volumes_data[i].get_instance_position());
            }
            else if (m_mode == Volume)
            {
                volume.set_volume_rotation(m_cache.volumes_data[i].get_volume_rotation());
                volume.set_volume_offset(m_cache.volumes_data[i].get_volume_position());
            }
        }
    }
    else
    {
        //FIXME this does not work for absolute rotations (transformation_type.absolute() is true)
        rotation.cwiseAbs().maxCoeff(&rot_axis_max);

        // For generic rotation, we want to rotate the first volume in selection, and then to synchronize the other volumes with it.
        std::vector<int> object_instance_first(m_model->objects.size(), -1);
        auto rotate_instance = [this, &rotation, &object_instance_first, rot_axis_max, transformation_type](GLVolume &volume, int i) {
            int first_volume_idx = object_instance_first[volume.object_idx()];
            if (rot_axis_max != 2 && first_volume_idx != -1) {
                // Generic rotation, but no rotation around the Z axis.
                // Always do a local rotation (do not consider the selection to be a rigid body).
                assert(is_approx(rotation.z(), 0.0));
                const GLVolume &first_volume = *(*m_volumes)[first_volume_idx];
                const Vec3d    &rotation = first_volume.get_instance_rotation();
                double z_diff = rotation_diff_z(m_cache.volumes_data[first_volume_idx].get_instance_rotation(), m_cache.volumes_data[i].get_instance_rotation());
                volume.set_instance_rotation(Vec3d(rotation(0), rotation(1), rotation(2) + z_diff));
            }
            else {
                // extracts rotations from the composed transformation
                Vec3d new_rotation = transformation_type.world() ?
                    Geometry::extract_euler_angles(Geometry::assemble_transform(Vec3d::Zero(), rotation) * m_cache.volumes_data[i].get_instance_rotation_matrix()) :
                    transformation_type.absolute() ? rotation : rotation + m_cache.volumes_data[i].get_instance_rotation();
                if (rot_axis_max == 2 && transformation_type.joint()) {
                    // Only allow rotation of multiple instances as a single rigid body when rotating around the Z axis.
                    Vec3d offset = Geometry::assemble_transform(Vec3d::Zero(), Vec3d(0.0, 0.0, new_rotation(2) - m_cache.volumes_data[i].get_instance_rotation()(2))) * (m_cache.volumes_data[i].get_instance_position() - m_cache.dragging_center);
                    volume.set_instance_offset(m_cache.dragging_center + offset);
                }
                volume.set_instance_rotation(new_rotation);
                object_instance_first[volume.object_idx()] = i;
            }
        };

        for (unsigned int i : m_list)
        {
            GLVolume &volume = *(*m_volumes)[i];
            if (is_single_full_instance())
                rotate_instance(volume, i);
            else if (is_single_volume() || is_single_modifier())
            {
                if (transformation_type.independent())
                    volume.set_volume_rotation(volume.get_volume_rotation() + rotation);
                else
                {
                    Transform3d m = Geometry::assemble_transform(Vec3d::Zero(), rotation);
                    Vec3d new_rotation = Geometry::extract_euler_angles(m * m_cache.volumes_data[i].get_volume_rotation_matrix());
                    volume.set_volume_rotation(new_rotation);
                }
            }
            else
            {
                if (m_mode == Instance)
                    rotate_instance(volume, i);
                else if (m_mode == Volume)
                {
                    // extracts rotations from the composed transformation
                    Transform3d m = Geometry::assemble_transform(Vec3d::Zero(), rotation);
                    Vec3d new_rotation = Geometry::extract_euler_angles(m * m_cache.volumes_data[i].get_volume_rotation_matrix());
                    if (transformation_type.joint())
                    {
                        Vec3d local_pivot = m_cache.volumes_data[i].get_instance_full_matrix().inverse() * m_cache.dragging_center;
                        Vec3d offset = m * (m_cache.volumes_data[i].get_volume_position() - local_pivot);
                        volume.set_volume_offset(local_pivot + offset);
                    }
                    volume.set_volume_rotation(new_rotation);
                }
            }
        }
    }

#if !DISABLE_INSTANCES_SYNCH
    if (m_mode == Instance)
        _synchronize_unselected_instances((rot_axis_max == 2) ? SYNC_ROTATION_NONE : SYNC_ROTATION_GENERAL);
    else if (m_mode == Volume)
        _synchronize_unselected_volumes();
#endif // !DISABLE_INSTANCES_SYNCH

    m_bounding_box_dirty = true;
}

void Selection::flattening_rotate(const Vec3d& normal)
{
    // We get the normal in untransformed coordinates. We must transform it using the instance matrix, find out
    // how to rotate the instance so it faces downwards and do the rotation. All that for all selected instances.
    // The function assumes that is_from_single_object() holds.

    if (!m_valid)
        return;

    for (unsigned int i : m_list)
    {
        Transform3d wst = m_cache.volumes_data[i].get_instance_scale_matrix();
        Vec3d scaling_factor = Vec3d(1. / wst(0, 0), 1. / wst(1, 1), 1. / wst(2, 2));

        Transform3d wmt = m_cache.volumes_data[i].get_instance_mirror_matrix();
        Vec3d mirror(wmt(0, 0), wmt(1, 1), wmt(2, 2));

        Vec3d rotation = Geometry::extract_euler_angles(m_cache.volumes_data[i].get_instance_rotation_matrix());
        Vec3d transformed_normal = Geometry::assemble_transform(Vec3d::Zero(), rotation, scaling_factor, mirror) * normal;
        transformed_normal.normalize();

        Vec3d axis = transformed_normal(2) > 0.999f ? Vec3d(1., 0., 0.) : Vec3d(transformed_normal.cross(Vec3d(0., 0., -1.)));
        axis.normalize();

        Transform3d extra_rotation = Transform3d::Identity();
        extra_rotation.rotate(Eigen::AngleAxisd(acos(-transformed_normal(2)), axis));

        Vec3d new_rotation = Geometry::extract_euler_angles(extra_rotation * m_cache.volumes_data[i].get_instance_rotation_matrix());
        (*m_volumes)[i]->set_instance_rotation(new_rotation);
    }

#if !DISABLE_INSTANCES_SYNCH
    // we want to synchronize z-rotation as well, otherwise the flattening behaves funny
    // when applied on one of several identical instances
    if (m_mode == Instance)
        _synchronize_unselected_instances(SYNC_ROTATION_FULL);
#endif // !DISABLE_INSTANCES_SYNCH

    m_bounding_box_dirty = true;
}

void Selection::scale(const Vec3d& scale, bool local)
{
    if (!m_valid)
        return;

    for (unsigned int i : m_list)
    {
        if (is_single_full_instance())
            (*m_volumes)[i]->set_instance_scaling_factor(scale);
        else if (is_single_volume() || is_single_modifier())
            (*m_volumes)[i]->set_volume_scaling_factor(scale);
        else
        {
            Transform3d m = Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), scale);
            if (m_mode == Instance)
            {
                Eigen::Matrix<double, 3, 3, Eigen::DontAlign> new_matrix = (m * m_cache.volumes_data[i].get_instance_scale_matrix()).matrix().block(0, 0, 3, 3);
                // extracts scaling factors from the composed transformation
                Vec3d new_scale(new_matrix.col(0).norm(), new_matrix.col(1).norm(), new_matrix.col(2).norm());
                if (!local)
                    (*m_volumes)[i]->set_instance_offset(m_cache.dragging_center + m * (m_cache.volumes_data[i].get_instance_position() - m_cache.dragging_center));

                (*m_volumes)[i]->set_instance_scaling_factor(new_scale);
            }
            else if (m_mode == Volume)
            {
                Eigen::Matrix<double, 3, 3, Eigen::DontAlign> new_matrix = (m * m_cache.volumes_data[i].get_volume_scale_matrix()).matrix().block(0, 0, 3, 3);
                // extracts scaling factors from the composed transformation
                Vec3d new_scale(new_matrix.col(0).norm(), new_matrix.col(1).norm(), new_matrix.col(2).norm());
                if (!local)
                {
                    Vec3d offset = m * (m_cache.volumes_data[i].get_volume_position() + m_cache.volumes_data[i].get_instance_position() - m_cache.dragging_center);
                    (*m_volumes)[i]->set_volume_offset(m_cache.dragging_center - m_cache.volumes_data[i].get_instance_position() + offset);
                }
                (*m_volumes)[i]->set_volume_scaling_factor(new_scale);
            }
        }
    }

#if !DISABLE_INSTANCES_SYNCH
    if (m_mode == Instance)
        _synchronize_unselected_instances(SYNC_ROTATION_NONE);
    else if (m_mode == Volume)
        _synchronize_unselected_volumes();
#endif // !DISABLE_INSTANCES_SYNCH

    _ensure_on_bed();

    m_bounding_box_dirty = true;
}

void Selection::mirror(Axis axis)
{
    if (!m_valid)
        return;

    bool single_full_instance = is_single_full_instance();

    for (unsigned int i : m_list)
    {
        if (single_full_instance)
            (*m_volumes)[i]->set_instance_mirror(axis, -(*m_volumes)[i]->get_instance_mirror(axis));
        else if (m_mode == Volume)
            (*m_volumes)[i]->set_volume_mirror(axis, -(*m_volumes)[i]->get_volume_mirror(axis));
    }

#if !DISABLE_INSTANCES_SYNCH
    if (m_mode == Instance)
        _synchronize_unselected_instances(SYNC_ROTATION_NONE);
    else if (m_mode == Volume)
        _synchronize_unselected_volumes();
#endif // !DISABLE_INSTANCES_SYNCH

    m_bounding_box_dirty = true;
}

void Selection::translate(unsigned int object_idx, const Vec3d& displacement)
{
    if (!m_valid)
        return;

    for (unsigned int i : m_list)
    {
        GLVolume* v = (*m_volumes)[i];
        if (v->object_idx() == object_idx)
            v->set_instance_offset(v->get_instance_offset() + displacement);
    }

    std::set<unsigned int> done;  // prevent processing volumes twice
    done.insert(m_list.begin(), m_list.end());

    for (unsigned int i : m_list)
    {
        if (done.size() == m_volumes->size())
            break;

        int object_idx = (*m_volumes)[i]->object_idx();
        if (object_idx >= 1000)
            continue;

        // Process unselected volumes of the object.
        for (unsigned int j = 0; j < (unsigned int)m_volumes->size(); ++j)
        {
            if (done.size() == m_volumes->size())
                break;

            if (done.find(j) != done.end())
                continue;

            GLVolume* v = (*m_volumes)[j];
            if (v->object_idx() != object_idx)
                continue;

            v->set_instance_offset(v->get_instance_offset() + displacement);
            done.insert(j);
        }
    }

    m_bounding_box_dirty = true;
}

void Selection::translate(unsigned int object_idx, unsigned int instance_idx, const Vec3d& displacement)
{
    if (!m_valid)
        return;

    for (unsigned int i : m_list)
    {
        GLVolume* v = (*m_volumes)[i];
        if ((v->object_idx() == object_idx) && (v->instance_idx() == instance_idx))
            v->set_instance_offset(v->get_instance_offset() + displacement);
    }

    std::set<unsigned int> done;  // prevent processing volumes twice
    done.insert(m_list.begin(), m_list.end());

    for (unsigned int i : m_list)
    {
        if (done.size() == m_volumes->size())
            break;

        int object_idx = (*m_volumes)[i]->object_idx();
        if (object_idx >= 1000)
            continue;

        // Process unselected volumes of the object.
        for (unsigned int j = 0; j < (unsigned int)m_volumes->size(); ++j)
        {
            if (done.size() == m_volumes->size())
                break;

            if (done.find(j) != done.end())
                continue;

            GLVolume* v = (*m_volumes)[j];
            if ((v->object_idx() != object_idx) || (v->instance_idx() != instance_idx))
                continue;

            v->set_instance_offset(v->get_instance_offset() + displacement);
            done.insert(j);
        }
    }

    m_bounding_box_dirty = true;
}

void Selection::erase()
{
    if (!m_valid)
        return;

    if (is_single_full_object())
        wxGetApp().obj_list()->delete_from_model_and_list(ItemType::itObject, get_object_idx(), 0);
    else if (is_multiple_full_object())
    {
        std::vector<ItemForDelete> items;
        items.reserve(m_cache.content.size());
        for (ObjectIdxsToInstanceIdxsMap::iterator it = m_cache.content.begin(); it != m_cache.content.end(); ++it)
        {
            items.emplace_back(ItemType::itObject, it->first, 0);
        }
        wxGetApp().obj_list()->delete_from_model_and_list(items);
    }
    else if (is_multiple_full_instance())
    {
        std::set<std::pair<int, int>> instances_idxs;
        for (ObjectIdxsToInstanceIdxsMap::iterator obj_it = m_cache.content.begin(); obj_it != m_cache.content.end(); ++obj_it)
        {
            for (InstanceIdxsList::reverse_iterator inst_it = obj_it->second.rbegin(); inst_it != obj_it->second.rend(); ++inst_it)
            {
                instances_idxs.insert(std::make_pair(obj_it->first, *inst_it));
            }
        }

        std::vector<ItemForDelete> items;
        items.reserve(instances_idxs.size());
        for (const std::pair<int, int>& i : instances_idxs)
        {
            items.emplace_back(ItemType::itInstance, i.first, i.second);
        }
        wxGetApp().obj_list()->delete_from_model_and_list(items);
    }
    else if (is_single_full_instance())
        wxGetApp().obj_list()->delete_from_model_and_list(ItemType::itInstance, get_object_idx(), get_instance_idx());
    else if (is_mixed())
    {
        std::set<ItemForDelete> items_set;
        std::map<int, int> volumes_in_obj;

        for (auto i : m_list) {
            const auto gl_vol = (*m_volumes)[i];
            const auto glv_obj_idx = gl_vol->object_idx();
            const auto model_object = m_model->objects[glv_obj_idx];

            if (model_object->instances.size() == 1) {
                if (model_object->volumes.size() == 1)
                    items_set.insert(ItemForDelete(ItemType::itObject, glv_obj_idx, -1));
                else {
                    items_set.insert(ItemForDelete(ItemType::itVolume, glv_obj_idx, gl_vol->volume_idx()));
                    int idx = (volumes_in_obj.find(glv_obj_idx) == volumes_in_obj.end()) ? 0 : volumes_in_obj.at(glv_obj_idx);
                    volumes_in_obj[glv_obj_idx] = ++idx;
                }
                continue;
            }

            const auto glv_ins_idx = gl_vol->instance_idx();

            for (auto obj_ins : m_cache.content) {
                if (obj_ins.first == glv_obj_idx) {
                    if (obj_ins.second.find(glv_ins_idx) != obj_ins.second.end()) {
                        if (obj_ins.second.size() == model_object->instances.size())
                            items_set.insert(ItemForDelete(ItemType::itVolume, glv_obj_idx, gl_vol->volume_idx()));
                        else
                            items_set.insert(ItemForDelete(ItemType::itInstance, glv_obj_idx, glv_ins_idx));

                        break;
                    }
                }
            }
        }

        std::vector<ItemForDelete> items;
        items.reserve(items_set.size());
        for (const ItemForDelete& i : items_set) {
            if (i.type == ItemType::itVolume) {
                const int vol_in_obj_cnt = volumes_in_obj.find(i.obj_idx) == volumes_in_obj.end() ? 0 : volumes_in_obj.at(i.obj_idx);
                if (vol_in_obj_cnt == m_model->objects[i.obj_idx]->volumes.size()) {
                    if (i.sub_obj_idx == vol_in_obj_cnt - 1)
                        items.emplace_back(ItemType::itObject, i.obj_idx, 0);
                    continue;
                }
            }
            items.emplace_back(i.type, i.obj_idx, i.sub_obj_idx);
        }

        wxGetApp().obj_list()->delete_from_model_and_list(items);
    }
    else
    {
        std::set<std::pair<int, int>> volumes_idxs;
        for (unsigned int i : m_list)
        {
            const GLVolume* v = (*m_volumes)[i];
            // Only remove volumes associated with ModelVolumes from the object list.
            // Temporary meshes (SLA supports or pads) are not managed by the object list.
            if (v->volume_idx() >= 0)
                volumes_idxs.insert(std::make_pair(v->object_idx(), v->volume_idx()));
        }

        std::vector<ItemForDelete> items;
        items.reserve(volumes_idxs.size());
        for (const std::pair<int, int>& v : volumes_idxs)
        {
            items.emplace_back(ItemType::itVolume, v.first, v.second);
        }

        wxGetApp().obj_list()->delete_from_model_and_list(items);
    }
}

void Selection::render(float scale_factor) const
{
    if (!m_valid || is_empty())
        return;

    m_scale_factor = scale_factor;

    // render cumulative bounding box of selected volumes
    _render_selected_volumes();
    _render_synchronized_volumes();
}

#if ENABLE_RENDER_SELECTION_CENTER
void Selection::render_center() const
{
    if (!m_valid || is_empty() || (m_quadric == nullptr))
        return;

    const Vec3d& center = get_bounding_box().center();

    ::glDisable(GL_DEPTH_TEST);

    ::glEnable(GL_LIGHTING);

    ::glColor3f(1.0f, 1.0f, 1.0f);
    ::glPushMatrix();
    ::glTranslated(center(0), center(1), center(2));
    ::gluSphere(m_quadric, 0.75, 32, 32);
    ::glPopMatrix();

    ::glDisable(GL_LIGHTING);
}
#endif // ENABLE_RENDER_SELECTION_CENTER

void Selection::render_sidebar_hints(const std::string& sidebar_field) const
{
    if (sidebar_field.empty())
        return;

    ::glClear(GL_DEPTH_BUFFER_BIT);
    ::glEnable(GL_DEPTH_TEST);

    ::glEnable(GL_LIGHTING);

    ::glPushMatrix();

    const Vec3d& center = get_bounding_box().center();

    if (is_single_full_instance())
    {
        ::glTranslated(center(0), center(1), center(2));
        if (!boost::starts_with(sidebar_field, "position"))
        {
            Transform3d orient_matrix = (*m_volumes)[*m_list.begin()]->get_instance_transformation().get_matrix(true, false, true, true);
            ::glMultMatrixd(orient_matrix.data());
        }
    }
    else if (is_single_volume() || is_single_modifier())
    {
        ::glTranslated(center(0), center(1), center(2));
        Transform3d orient_matrix = (*m_volumes)[*m_list.begin()]->get_instance_transformation().get_matrix(true, false, true, true);
        if (!boost::starts_with(sidebar_field, "position"))
            orient_matrix = orient_matrix * (*m_volumes)[*m_list.begin()]->get_volume_transformation().get_matrix(true, false, true, true);

        ::glMultMatrixd(orient_matrix.data());
    }
    else
    {
        ::glTranslated(center(0), center(1), center(2));
        if (requires_local_axes())
        {
            Transform3d orient_matrix = (*m_volumes)[*m_list.begin()]->get_instance_transformation().get_matrix(true, false, true, true);
            ::glMultMatrixd(orient_matrix.data());
        }
    }

    if (boost::starts_with(sidebar_field, "position"))
        _render_sidebar_position_hints(sidebar_field);
    else if (boost::starts_with(sidebar_field, "rotation"))
        _render_sidebar_rotation_hints(sidebar_field);
    else if (boost::starts_with(sidebar_field, "scale"))
        _render_sidebar_scale_hints(sidebar_field);
    else if (boost::starts_with(sidebar_field, "size"))
        _render_sidebar_size_hints(sidebar_field);

    ::glPopMatrix();

    ::glDisable(GL_LIGHTING);
}

bool Selection::requires_local_axes() const
{
    return (m_mode == Volume) && is_from_single_instance();
}

void Selection::_update_valid()
{
    m_valid = (m_volumes != nullptr) && (m_model != nullptr);
}

void Selection::_update_type()
{
    m_cache.content.clear();
    m_type = Mixed;

    for (unsigned int i : m_list)
    {
        const GLVolume* volume = (*m_volumes)[i];
        int obj_idx = volume->object_idx();
        int inst_idx = volume->instance_idx();
        ObjectIdxsToInstanceIdxsMap::iterator obj_it = m_cache.content.find(obj_idx);
        if (obj_it == m_cache.content.end())
            obj_it = m_cache.content.insert(ObjectIdxsToInstanceIdxsMap::value_type(obj_idx, InstanceIdxsList())).first;

        obj_it->second.insert(inst_idx);
    }

    bool requires_disable = false;

    if (!m_valid)
        m_type = Invalid;
    else
    {
        if (m_list.empty())
            m_type = Empty;
        else if (m_list.size() == 1)
        {
            const GLVolume* first = (*m_volumes)[*m_list.begin()];
            if (first->is_wipe_tower)
                m_type = WipeTower;
            else if (first->is_modifier)
            {
                m_type = SingleModifier;
                requires_disable = true;
            }
            else
            {
                const ModelObject* model_object = m_model->objects[first->object_idx()];
                unsigned int volumes_count = (unsigned int)model_object->volumes.size();
                unsigned int instances_count = (unsigned int)model_object->instances.size();
                if (volumes_count * instances_count == 1)
                {
                    m_type = SingleFullObject;
                    // ensures the correct mode is selected
                    m_mode = Instance;
                }
                else if (volumes_count == 1) // instances_count > 1
                {
                    m_type = SingleFullInstance;
                    // ensures the correct mode is selected
                    m_mode = Instance;
                }
                else
                {
                    m_type = SingleVolume;
                    requires_disable = true;
                }
            }
        }
        else
        {
            if (m_cache.content.size() == 1) // single object
            {
                const ModelObject* model_object = m_model->objects[m_cache.content.begin()->first];
                unsigned int model_volumes_count = (unsigned int)model_object->volumes.size();
                unsigned int sla_volumes_count = 0;
                for (unsigned int i : m_list)
                {
                    if ((*m_volumes)[i]->volume_idx() < 0)
                        ++sla_volumes_count;
                }
                unsigned int volumes_count = model_volumes_count + sla_volumes_count;
                unsigned int instances_count = (unsigned int)model_object->instances.size();
                unsigned int selected_instances_count = (unsigned int)m_cache.content.begin()->second.size();
                if (volumes_count * instances_count == (unsigned int)m_list.size())
                {
                    m_type = SingleFullObject;
                    // ensures the correct mode is selected
                    m_mode = Instance;
                }
                else if (selected_instances_count == 1)
                {
                    if (volumes_count == (unsigned int)m_list.size())
                    {
                        m_type = SingleFullInstance;
                        // ensures the correct mode is selected
                        m_mode = Instance;
                    }
                    else
                    {
                        unsigned int modifiers_count = 0;
                        for (unsigned int i : m_list)
                        {
                            if ((*m_volumes)[i]->is_modifier)
                                ++modifiers_count;
                        }

                        if (modifiers_count == 0)
                        {
                            m_type = MultipleVolume;
                            requires_disable = true;
                        }
                        else if (modifiers_count == (unsigned int)m_list.size())
                        {
                            m_type = MultipleModifier;
                            requires_disable = true;
                        }
                    }
                }
                else if ((selected_instances_count > 1) && (selected_instances_count * volumes_count == (unsigned int)m_list.size()))
                {
                    m_type = MultipleFullInstance;
                    // ensures the correct mode is selected
                    m_mode = Instance;
                }
            }
            else
            {
                int sels_cntr = 0;
                for (ObjectIdxsToInstanceIdxsMap::iterator it = m_cache.content.begin(); it != m_cache.content.end(); ++it)
                {
                    const ModelObject* model_object = m_model->objects[it->first];
                    unsigned int volumes_count = (unsigned int)model_object->volumes.size();
                    unsigned int instances_count = (unsigned int)model_object->instances.size();
                    sels_cntr += volumes_count * instances_count;
                }
                if (sels_cntr == (unsigned int)m_list.size())
                {
                    m_type = MultipleFullObject;
                    // ensures the correct mode is selected
                    m_mode = Instance;
                }
            }
        }
    }

    int object_idx = get_object_idx();
    int instance_idx = get_instance_idx();
    for (GLVolume* v : *m_volumes)
    {
        v->disabled = requires_disable ? (v->object_idx() != object_idx) || (v->instance_idx() != instance_idx) : false;
    }

#if ENABLE_SELECTION_DEBUG_OUTPUT
    std::cout << "Selection: ";
    std::cout << "mode: ";
    switch (m_mode)
    {
    case Volume:
    {
        std::cout << "Volume";
        break;
    }
    case Instance:
    {
        std::cout << "Instance";
        break;
    }
    }

    std::cout << " - type: ";

    switch (m_type)
    {
    case Invalid:
    {
        std::cout << "Invalid" << std::endl;
        break;
    }
    case Empty:
    {
        std::cout << "Empty" << std::endl;
        break;
    }
    case WipeTower:
    {
        std::cout << "WipeTower" << std::endl;
        break;
    }
    case SingleModifier:
    {
        std::cout << "SingleModifier" << std::endl;
        break;
    }
    case MultipleModifier:
    {
        std::cout << "MultipleModifier" << std::endl;
        break;
    }
    case SingleVolume:
    {
        std::cout << "SingleVolume" << std::endl;
        break;
    }
    case MultipleVolume:
    {
        std::cout << "MultipleVolume" << std::endl;
        break;
    }
    case SingleFullObject:
    {
        std::cout << "SingleFullObject" << std::endl;
        break;
    }
    case MultipleFullObject:
    {
        std::cout << "MultipleFullObject" << std::endl;
        break;
    }
    case SingleFullInstance:
    {
        std::cout << "SingleFullInstance" << std::endl;
        break;
    }
    case MultipleFullInstance:
    {
        std::cout << "MultipleFullInstance" << std::endl;
        break;
    }
    case Mixed:
    {
        std::cout << "Mixed" << std::endl;
        break;
    }
    }
#endif // ENABLE_SELECTION_DEBUG_OUTPUT
}

void Selection::_set_caches()
{
    m_cache.volumes_data.clear();
    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i)
    {
        const GLVolume* v = (*m_volumes)[i];
        m_cache.volumes_data.emplace(i, VolumeCache(v->get_volume_transformation(), v->get_instance_transformation()));
    }
    m_cache.dragging_center = get_bounding_box().center();
}

void Selection::_add_volume(unsigned int volume_idx)
{
    m_list.insert(volume_idx);
    (*m_volumes)[volume_idx]->selected = true;
}

void Selection::_add_instance(unsigned int object_idx, unsigned int instance_idx)
{
    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i)
    {
        GLVolume* v = (*m_volumes)[i];
        if ((v->object_idx() == object_idx) && (v->instance_idx() == instance_idx))
            _add_volume(i);
    }
}

void Selection::_add_object(unsigned int object_idx)
{
    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i)
    {
        GLVolume* v = (*m_volumes)[i];
        if (v->object_idx() == object_idx)
            _add_volume(i);
    }
}

void Selection::_remove_volume(unsigned int volume_idx)
{
    IndicesList::iterator v_it = m_list.find(volume_idx);
    if (v_it == m_list.end())
        return;

    m_list.erase(v_it);

    (*m_volumes)[volume_idx]->selected = false;
}

void Selection::_remove_instance(unsigned int object_idx, unsigned int instance_idx)
{
    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i)
    {
        GLVolume* v = (*m_volumes)[i];
        if ((v->object_idx() == object_idx) && (v->instance_idx() == instance_idx))
            _remove_volume(i);
    }
}

void Selection::_remove_object(unsigned int object_idx)
{
    for (unsigned int i = 0; i < (unsigned int)m_volumes->size(); ++i)
    {
        GLVolume* v = (*m_volumes)[i];
        if (v->object_idx() == object_idx)
            _remove_volume(i);
    }
}

void Selection::_calc_bounding_box() const
{
    m_bounding_box = BoundingBoxf3();
    if (m_valid)
    {
        for (unsigned int i : m_list)
        {
            m_bounding_box.merge((*m_volumes)[i]->transformed_convex_hull_bounding_box());
        }
    }
    m_bounding_box_dirty = false;
}

void Selection::_render_selected_volumes() const
{
    float color[3] = { 1.0f, 1.0f, 1.0f };
    _render_bounding_box(get_bounding_box(), color);
}

void Selection::_render_synchronized_volumes() const
{
    if (m_mode == Instance)
        return;

    float color[3] = { 1.0f, 1.0f, 0.0f };

    for (unsigned int i : m_list)
    {
        const GLVolume* volume = (*m_volumes)[i];
        int object_idx = volume->object_idx();
        int instance_idx = volume->instance_idx();
        int volume_idx = volume->volume_idx();
        for (unsigned int j = 0; j < (unsigned int)m_volumes->size(); ++j)
        {
            if (i == j)
                continue;

            const GLVolume* v = (*m_volumes)[j];
            if ((v->object_idx() != object_idx) || (v->volume_idx() != volume_idx))
                continue;

            _render_bounding_box(v->transformed_convex_hull_bounding_box(), color);
        }
    }
}

void Selection::_render_bounding_box(const BoundingBoxf3& box, float* color) const
{
    if (color == nullptr)
        return;

    Vec3f b_min = box.min.cast<float>();
    Vec3f b_max = box.max.cast<float>();
    Vec3f size = 0.2f * box.size().cast<float>();

    ::glEnable(GL_DEPTH_TEST);

    ::glColor3fv(color);
    ::glLineWidth(2.0f * m_scale_factor);

    ::glBegin(GL_LINES);

    ::glVertex3f(b_min(0), b_min(1), b_min(2)); ::glVertex3f(b_min(0) + size(0), b_min(1), b_min(2));
    ::glVertex3f(b_min(0), b_min(1), b_min(2)); ::glVertex3f(b_min(0), b_min(1) + size(1), b_min(2));
    ::glVertex3f(b_min(0), b_min(1), b_min(2)); ::glVertex3f(b_min(0), b_min(1), b_min(2) + size(2));

    ::glVertex3f(b_max(0), b_min(1), b_min(2)); ::glVertex3f(b_max(0) - size(0), b_min(1), b_min(2));
    ::glVertex3f(b_max(0), b_min(1), b_min(2)); ::glVertex3f(b_max(0), b_min(1) + size(1), b_min(2));
    ::glVertex3f(b_max(0), b_min(1), b_min(2)); ::glVertex3f(b_max(0), b_min(1), b_min(2) + size(2));

    ::glVertex3f(b_max(0), b_max(1), b_min(2)); ::glVertex3f(b_max(0) - size(0), b_max(1), b_min(2));
    ::glVertex3f(b_max(0), b_max(1), b_min(2)); ::glVertex3f(b_max(0), b_max(1) - size(1), b_min(2));
    ::glVertex3f(b_max(0), b_max(1), b_min(2)); ::glVertex3f(b_max(0), b_max(1), b_min(2) + size(2));

    ::glVertex3f(b_min(0), b_max(1), b_min(2)); ::glVertex3f(b_min(0) + size(0), b_max(1), b_min(2));
    ::glVertex3f(b_min(0), b_max(1), b_min(2)); ::glVertex3f(b_min(0), b_max(1) - size(1), b_min(2));
    ::glVertex3f(b_min(0), b_max(1), b_min(2)); ::glVertex3f(b_min(0), b_max(1), b_min(2) + size(2));

    ::glVertex3f(b_min(0), b_min(1), b_max(2)); ::glVertex3f(b_min(0) + size(0), b_min(1), b_max(2));
    ::glVertex3f(b_min(0), b_min(1), b_max(2)); ::glVertex3f(b_min(0), b_min(1) + size(1), b_max(2));
    ::glVertex3f(b_min(0), b_min(1), b_max(2)); ::glVertex3f(b_min(0), b_min(1), b_max(2) - size(2));

    ::glVertex3f(b_max(0), b_min(1), b_max(2)); ::glVertex3f(b_max(0) - size(0), b_min(1), b_max(2));
    ::glVertex3f(b_max(0), b_min(1), b_max(2)); ::glVertex3f(b_max(0), b_min(1) + size(1), b_max(2));
    ::glVertex3f(b_max(0), b_min(1), b_max(2)); ::glVertex3f(b_max(0), b_min(1), b_max(2) - size(2));

    ::glVertex3f(b_max(0), b_max(1), b_max(2)); ::glVertex3f(b_max(0) - size(0), b_max(1), b_max(2));
    ::glVertex3f(b_max(0), b_max(1), b_max(2)); ::glVertex3f(b_max(0), b_max(1) - size(1), b_max(2));
    ::glVertex3f(b_max(0), b_max(1), b_max(2)); ::glVertex3f(b_max(0), b_max(1), b_max(2) - size(2));

    ::glVertex3f(b_min(0), b_max(1), b_max(2)); ::glVertex3f(b_min(0) + size(0), b_max(1), b_max(2));
    ::glVertex3f(b_min(0), b_max(1), b_max(2)); ::glVertex3f(b_min(0), b_max(1) - size(1), b_max(2));
    ::glVertex3f(b_min(0), b_max(1), b_max(2)); ::glVertex3f(b_min(0), b_max(1), b_max(2) - size(2));

    ::glEnd();
}

void Selection::_render_sidebar_position_hints(const std::string& sidebar_field) const
{
    if (boost::ends_with(sidebar_field, "x"))
    {
        ::glRotated(-90.0, 0.0, 0.0, 1.0);
        _render_sidebar_position_hint(X);
    }
    else if (boost::ends_with(sidebar_field, "y"))
        _render_sidebar_position_hint(Y);
    else if (boost::ends_with(sidebar_field, "z"))
    {
        ::glRotated(90.0, 1.0, 0.0, 0.0);
        _render_sidebar_position_hint(Z);
    }
}

void Selection::_render_sidebar_rotation_hints(const std::string& sidebar_field) const
{
    if (boost::ends_with(sidebar_field, "x"))
    {
        ::glRotated(90.0, 0.0, 1.0, 0.0);
        _render_sidebar_rotation_hint(X);
    }
    else if (boost::ends_with(sidebar_field, "y"))
    {
        ::glRotated(-90.0, 1.0, 0.0, 0.0);
        _render_sidebar_rotation_hint(Y);
    }
    else if (boost::ends_with(sidebar_field, "z"))
        _render_sidebar_rotation_hint(Z);
}

void Selection::_render_sidebar_scale_hints(const std::string& sidebar_field) const
{
    bool uniform_scale = requires_uniform_scale() || wxGetApp().obj_manipul()->get_uniform_scaling();

    if (boost::ends_with(sidebar_field, "x") || uniform_scale)
    {
        ::glPushMatrix();
        ::glRotated(-90.0, 0.0, 0.0, 1.0);
        _render_sidebar_scale_hint(X);
        ::glPopMatrix();
    }

    if (boost::ends_with(sidebar_field, "y") || uniform_scale)
    {
        ::glPushMatrix();
        _render_sidebar_scale_hint(Y);
        ::glPopMatrix();
    }

    if (boost::ends_with(sidebar_field, "z") || uniform_scale)
    {
        ::glPushMatrix();
        ::glRotated(90.0, 1.0, 0.0, 0.0);
        _render_sidebar_scale_hint(Z);
        ::glPopMatrix();
    }
}

void Selection::_render_sidebar_size_hints(const std::string& sidebar_field) const
{
    _render_sidebar_scale_hints(sidebar_field);
}

void Selection::_render_sidebar_position_hint(Axis axis) const
{
    m_arrow.set_color(AXES_COLOR[axis], 3);
    m_arrow.render();
}

void Selection::_render_sidebar_rotation_hint(Axis axis) const
{
    m_curved_arrow.set_color(AXES_COLOR[axis], 3);
    m_curved_arrow.render();

    ::glRotated(180.0, 0.0, 0.0, 1.0);
    m_curved_arrow.render();
}

void Selection::_render_sidebar_scale_hint(Axis axis) const
{
    m_arrow.set_color(((requires_uniform_scale() || wxGetApp().obj_manipul()->get_uniform_scaling()) ? UNIFORM_SCALE_COLOR : AXES_COLOR[axis]), 3);

    ::glTranslated(0.0, 5.0, 0.0);
    m_arrow.render();

    ::glTranslated(0.0, -10.0, 0.0);
    ::glRotated(180.0, 0.0, 0.0, 1.0);
    m_arrow.render();
}

void Selection::_render_sidebar_size_hint(Axis axis, double length) const
{
}

#ifndef NDEBUG
static bool is_rotation_xy_synchronized(const Vec3d &rot_xyz_from, const Vec3d &rot_xyz_to)
{
    Eigen::AngleAxisd angle_axis(rotation_xyz_diff(rot_xyz_from, rot_xyz_to));
    Vec3d  axis = angle_axis.axis();
    double angle = angle_axis.angle();
    if (std::abs(angle) < 1e-8)
        return true;
    assert(std::abs(axis.x()) < 1e-8);
    assert(std::abs(axis.y()) < 1e-8);
    assert(std::abs(std::abs(axis.z()) - 1.) < 1e-8);
    return std::abs(axis.x()) < 1e-8 && std::abs(axis.y()) < 1e-8 && std::abs(std::abs(axis.z()) - 1.) < 1e-8;
}

static void verify_instances_rotation_synchronized(const Model &model, const GLVolumePtrs &volumes)
{
    for (size_t idx_object = 0; idx_object < model.objects.size(); ++idx_object) {
        int idx_volume_first = -1;
        for (int i = 0; i < (int)volumes.size(); ++i) {
            if (volumes[i]->object_idx() == idx_object) {
                idx_volume_first = i;
                break;
            }
        }
        assert(idx_volume_first != -1); // object without instances?
        if (idx_volume_first == -1)
            continue;
        const Vec3d &rotation0 = volumes[idx_volume_first]->get_instance_rotation();
        for (int i = idx_volume_first + 1; i < (int)volumes.size(); ++i)
            if (volumes[i]->object_idx() == idx_object) {
                const Vec3d &rotation = volumes[i]->get_instance_rotation();
                assert(is_rotation_xy_synchronized(rotation, rotation0));
            }
    }
}
#endif /* NDEBUG */

void Selection::_synchronize_unselected_instances(SyncRotationType sync_rotation_type)
{
    std::set<unsigned int> done;  // prevent processing volumes twice
    done.insert(m_list.begin(), m_list.end());

    for (unsigned int i : m_list)
    {
        if (done.size() == m_volumes->size())
            break;

        const GLVolume* volume = (*m_volumes)[i];
        int object_idx = volume->object_idx();
        if (object_idx >= 1000)
            continue;

        int instance_idx = volume->instance_idx();
        const Vec3d& rotation = volume->get_instance_rotation();
        const Vec3d& scaling_factor = volume->get_instance_scaling_factor();
        const Vec3d& mirror = volume->get_instance_mirror();

        // Process unselected instances.
        for (unsigned int j = 0; j < (unsigned int)m_volumes->size(); ++j)
        {
            if (done.size() == m_volumes->size())
                break;

            if (done.find(j) != done.end())
                continue;

            GLVolume* v = (*m_volumes)[j];
            if ((v->object_idx() != object_idx) || (v->instance_idx() == instance_idx))
                continue;

            assert(is_rotation_xy_synchronized(m_cache.volumes_data[i].get_instance_rotation(), m_cache.volumes_data[j].get_instance_rotation()));
            switch (sync_rotation_type) {
            case SYNC_ROTATION_NONE:
                // z only rotation -> keep instance z
                // The X,Y rotations should be synchronized from start to end of the rotation.
                assert(is_rotation_xy_synchronized(rotation, v->get_instance_rotation()));
                break;
            case SYNC_ROTATION_FULL:
                // rotation comes from place on face -> force given z
                v->set_instance_rotation(Vec3d(rotation(0), rotation(1), rotation(2)));
                break;
            case SYNC_ROTATION_GENERAL:
                // generic rotation -> update instance z with the delta of the rotation.
                double z_diff = rotation_diff_z(m_cache.volumes_data[i].get_instance_rotation(), m_cache.volumes_data[j].get_instance_rotation());
                v->set_instance_rotation(Vec3d(rotation(0), rotation(1), rotation(2) + z_diff));
                break;
            }

            v->set_instance_scaling_factor(scaling_factor);
            v->set_instance_mirror(mirror);

            done.insert(j);
        }
    }

#ifndef NDEBUG
    verify_instances_rotation_synchronized(*m_model, *m_volumes);
#endif /* NDEBUG */
}

void Selection::_synchronize_unselected_volumes()
{
    for (unsigned int i : m_list)
    {
        const GLVolume* volume = (*m_volumes)[i];
        int object_idx = volume->object_idx();
        if (object_idx >= 1000)
            continue;

        int volume_idx = volume->volume_idx();
        const Vec3d& offset = volume->get_volume_offset();
        const Vec3d& rotation = volume->get_volume_rotation();
        const Vec3d& scaling_factor = volume->get_volume_scaling_factor();
        const Vec3d& mirror = volume->get_volume_mirror();

        // Process unselected volumes.
        for (unsigned int j = 0; j < (unsigned int)m_volumes->size(); ++j)
        {
            if (j == i)
                continue;

            GLVolume* v = (*m_volumes)[j];
            if ((v->object_idx() != object_idx) || (v->volume_idx() != volume_idx))
                continue;

            v->set_volume_offset(offset);
            v->set_volume_rotation(rotation);
            v->set_volume_scaling_factor(scaling_factor);
            v->set_volume_mirror(mirror);
        }
    }
}

void Selection::_ensure_on_bed()
{
    typedef std::map<std::pair<int, int>, double> InstancesToZMap;
    InstancesToZMap instances_min_z;

    for (GLVolume* volume : *m_volumes)
    {
        if (!volume->is_wipe_tower && !volume->is_modifier)
        {
            double min_z = volume->transformed_convex_hull_bounding_box().min(2);
            std::pair<int, int> instance = std::make_pair(volume->object_idx(), volume->instance_idx());
            InstancesToZMap::iterator it = instances_min_z.find(instance);
            if (it == instances_min_z.end())
                it = instances_min_z.insert(InstancesToZMap::value_type(instance, DBL_MAX)).first;

            it->second = std::min(it->second, min_z);
        }
    }

    for (GLVolume* volume : *m_volumes)
    {
        std::pair<int, int> instance = std::make_pair(volume->object_idx(), volume->instance_idx());
        InstancesToZMap::iterator it = instances_min_z.find(instance);
        if (it != instances_min_z.end())
            volume->set_instance_offset(Z, volume->get_instance_offset(Z) - it->second);
    }
}

} // namespace GUI
} // namespace Slic3r
