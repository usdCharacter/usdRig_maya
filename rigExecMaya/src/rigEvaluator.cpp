#include "rigEvaluator.h"
#include "taskPool.h"

#include "rigExecRuntime/runtime.h"

#include <maya/MProfiler.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstring>
#include <limits>
#include <map>

namespace rigExecMaya {

namespace {

void
Multiply(const double a[16], const double b[16], double out[16])
{
    double result[16];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += a[r * 4 + k] * b[k * 4 + c];
            }
            result[r * 4 + c] = sum;
        }
    }
    std::memcpy(out, result, sizeof(result));
}

void
CopyMatrix(const rigExec::RrMat4d &m, double out[16])
{
    std::memcpy(out, m._mtx, 16 * sizeof(double));
}

// A published point frame (origin, then the tips of its three axes) as a
// row-vector matrix: the three axis vectors, then the origin.
void
FrameMatrix(const rigExec::RrPointFrame &frame, double out[16])
{
    const rigExec::RrVec3d &origin = frame.points[0];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r * 4 + c] = frame.points[size_t(r + 1)][c] - origin[c];
        }
        out[r * 4 + 3] = 0.0;
    }
    for (int c = 0; c < 3; ++c) {
        out[12 + c] = origin[c];
    }
    out[15] = 1.0;
}

const double kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

}  // namespace

size_t
RigSetup::PointOffset(size_t mesh) const
{
    size_t offset = 0;
    for (size_t m = 0; m < mesh; ++m) {
        offset += meshes[m].count * 12;
    }
    return offset;
}

size_t
RigSetup::ControlMatricesOffset() const
{
    return PointOffset(meshes.size());
}

size_t
RigSetup::MeshMatricesOffset() const
{
    return ControlMatricesOffset() + controlPaths.size() * 128;
}

size_t
RigSetup::ShaderMatricesOffset() const
{
    return MeshMatricesOffset() + meshes.size() * 128;
}

size_t
RigSetup::FrameSize() const
{
    return ShaderMatricesOffset() + shaderMatrices.size() * 128;
}

RigEvaluator::~RigEvaluator() = default;

std::unique_ptr<RigEvaluator>
RigEvaluator::Create(std::shared_ptr<const RigSetup> setup,
                     std::string *error)
{
    if (!setup || !setup->bytes || setup->bytes->empty()) {
        *error = "no rig file is loaded";
        return nullptr;
    }
    std::unique_ptr<RigEvaluator> evaluator(new RigEvaluator());
    evaluator->_reader = rigExec::RigExecRuntimeReader::Open(
        setup->bytes->data(), setup->bytes->size(), error);
    if (!evaluator->_reader) {
        *error = "could not open " + setup->file + ": " + *error;
        return nullptr;
    }
    // Which input each channel sets, found once: a channel the bake has no
    // input for would refuse every time.
    evaluator->_inputs.assign(setup->channelPaths.size(), -1);
    for (size_t i = 0; i < setup->channelPaths.size(); ++i) {
        size_t index = 0;
        if (evaluator->_reader->FindInput(setup->channelPaths[i], &index) &&
            evaluator->_reader->GetInputInfo(index).defaultCount == 0) {
            evaluator->_inputs[i] = long(index);
        } else {
            evaluator->_refusedPaths.push_back(setup->channelPaths[i]);
        }
    }
    evaluator->_held = setup->bakedValues;
    // Where each published frame sits, by path: slots are fixed per file.
    const auto published = evaluator->_reader->GetProviderFramePublications();
    for (size_t slot = 0; slot < published.size(); ++slot) {
        evaluator->_frameSlots.emplace(published[slot].path, slot);
    }
    evaluator->_setup = std::move(setup);
    return evaluator;
}

bool
RigEvaluator::SetChannel(size_t input, double value)
{
    // The input's own type: a space or a switch may be an int or a bool.
    rigExec::RrInputValue typed = _reader->GetInputInfo(input).defaultValue;
    switch (typed.tag) {
    case rigExec::RrInputTag::Double: typed.f64 = value; break;
    case rigExec::RrInputTag::Float: typed.f32 = float(value); break;
    case rigExec::RrInputTag::Int: typed.i32 = int32_t(std::lround(value)); break;
    case rigExec::RrInputTag::Bool: typed.boolean = value != 0.0; break;
    default: return false;
    }
    return _reader->SetInputAt(input, typed, nullptr);
}

bool
RigEvaluator::_Run(
    const std::vector<double> &values,
    const std::vector<std::pair<std::string, double>> &overrides,
    std::string *error)
{
    const RigSetup &setup = *_setup;
    if (values.size() != setup.channelPaths.size()) {
        *error = "expected " + std::to_string(setup.channelPaths.size()) +
                 " channel values, got " + std::to_string(values.size());
        return false;
    }
    // Only what changed since the last run: the runtime recomputes what an
    // input it is handed feeds, so handing it every posed channel again
    // would recompute them all.
    for (const std::string &path : _overridden) {
        size_t index = 0;
        if (_reader->FindInput(path, &index)) {
            _reader->ResetInput(path, nullptr);
        }
    }
    _overridden.clear();
    for (size_t i = 0; i < values.size(); ++i) {
        if (_inputs[i] >= 0 && values[i] != _held[i]) {
            if (SetChannel(size_t(_inputs[i]), values[i])) {
                _held[i] = values[i];
            }
        }
    }
    for (const auto &[path, value] : overrides) {
        if (_reader->SetInput(path, value, nullptr)) {
            _overridden.push_back(path);
            // A channel overridden here is set again on the next run.
            for (size_t i = 0; i < setup.channelPaths.size(); ++i) {
                if (setup.channelPaths[i] == path) {
                    _held[i] = std::numeric_limits<double>::quiet_NaN();
                }
            }
        }
    }
    if (!_pool) {
        return _reader->Execute(error);
    }
    TaskPool *pool = _pool;
    _reader->SetTaskDispatch(
        [pool](std::function<void()> task) { pool->Run(std::move(task)); },
        [pool] { pool->Wait(); });
    const bool ran = _reader->Execute(error);
    _reader->SetTaskDispatch(nullptr, nullptr);
    return ran;
}

int
ProfilerCategory()
{
    static const int category =
        MProfiler::addCategory("RigExec", "The RigExec rig and its meshes");
    return category;
}

bool
RigEvaluator::Evaluate(const std::vector<double> &values,
                       std::vector<uint8_t> *frame, std::string *error)
{
    {
        MProfilingScope scope(ProfilerCategory(), MProfiler::kColorD_L2,
                              "rig runtime");
        if (!_Run(values, {}, error)) {
            return false;
        }
    }
    const RigSetup &setup = *_setup;
    frame->assign(setup.FrameSize(), 0);
    uint8_t *out = frame->data();

    MProfilingScope pointsScope(ProfilerCategory(), MProfiler::kColorD_L3,
                                "rig mesh points");
    const auto &moved = _reader->GetPoints();
    if (_pointSlots.empty()) {
        std::map<std::string, long> slots;
        for (size_t i = 0; i < moved.size(); ++i) {
            slots[moved[i].path] = long(i);
        }
        for (const RigSetup::Mesh &mesh : setup.meshes) {
            const auto found = slots.find(mesh.points);
            _pointSlots.push_back(found == slots.end() ? -1 : found->second);
        }
    }
    size_t offset = 0;
    for (size_t m = 0; m < setup.meshes.size(); ++m) {
        const RigSetup::Mesh &mesh = setup.meshes[m];
        const long slot = _pointSlots[m];
        if (slot >= 0 && size_t(slot) < moved.size() &&
            moved[size_t(slot)].points.size() == mesh.count) {
            std::memcpy(out + offset, moved[size_t(slot)].points.data(),
                        mesh.count * 12);
        } else if (mesh.rest.size() == mesh.count * 3) {
            std::memcpy(out + offset, mesh.rest.data(), mesh.count * 12);
        }
        offset += mesh.count * 12;
    }

    MProfilingScope controlsScope(ProfilerCategory(), MProfiler::kColorD_L3,
                                  "rig control frames");
    for (const std::string &path : setup.controlPaths) {
        double matrix[16];
        if (!_ControlFrame(path, matrix)) {
            std::memcpy(matrix, kIdentity, sizeof(matrix));
        }
        std::memcpy(out + offset, matrix, sizeof(matrix));
        offset += 128;
    }

    std::map<std::string, const rigExec::RrMat4d *> providers;
    for (const auto &xform : _reader->GetProviderXforms()) {
        providers[xform.path] = &xform.matrix;
    }
    for (const RigSetup::Mesh &mesh : setup.meshes) {
        double world[16];
        const auto found = mesh.provider.empty()
                               ? providers.end()
                               : providers.find(mesh.provider);
        if (found != providers.end()) {
            double provider[16];
            CopyMatrix(*found->second, provider);
            Multiply(mesh.local, provider, world);
            Multiply(world, setup.assetToWorld, world);
        } else {
            std::memcpy(world, mesh.world, sizeof(world));
        }
        std::memcpy(out + offset, world, sizeof(world));
        offset += 128;
    }

    if (!setup.shaderMatrices.empty()) {
        std::map<std::string, const rigExec::RrMat4d *> published;
        for (const auto &primvar : _reader->GetMatrixPrimvars()) {
            published[primvar.path] = &primvar.matrix;
        }
        for (const std::string &path : setup.shaderMatrices) {
            double matrix[16];
            const auto found = published.find(path);
            if (found != published.end()) {
                CopyMatrix(*found->second, matrix);
            } else {
                std::memcpy(matrix, kIdentity, sizeof(matrix));
            }
            std::memcpy(out + offset, matrix, sizeof(matrix));
            offset += 128;
        }
    }
    return true;
}

bool
RigEvaluator::Frames(
    const std::vector<double> &values,
    const std::vector<std::pair<std::string, double>> &overrides,
    const std::vector<std::string> &paths, std::vector<double> *frames,
    std::string *error)
{
    if (!_Run(values, overrides, error)) {
        return false;
    }
    std::map<std::string, rigExec::RigExecRuntimeJointMatrix> joints;
    frames->clear();
    for (const std::string &path : paths) {
        double matrix[16];
        if (!_ControlFrame(path, matrix)) {
            if (joints.empty()) {
                for (auto &joint : _reader->GetJointPoseMatrices()) {
                    joints[joint.path] = joint;
                }
            }
            const auto found = joints.find(path);
            if (found == joints.end()) {
                *error = "the rig has no control or joint " + path;
                return false;
            }
            CopyMatrix(found->second.matrix, matrix);
        }
        frames->insert(frames->end(), matrix, matrix + 16);
    }
    return true;
}

bool
RigEvaluator::Published(const std::vector<double> &values,
                        const std::string &path, std::vector<double> *out,
                        std::string *error)
{
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) {
        *error = "not a property path: " + path;
        return false;
    }
    const std::string prim = path.substr(0, dot);
    const std::string name = path.substr(dot + 1);
    if (!_Run(values, {}, error)) {
        return false;
    }
    if (name == "outputs:ikValues" || name == "outputs:fkValues") {
        for (const auto &match : _reader->GetIkFkMatches()) {
            if (match.path == prim) {
                *out = name == "outputs:ikValues" ? match.ikValues
                                                  : match.fkValues;
                return true;
            }
        }
    } else if (name == "spaces:matchValues") {
        for (const auto &match : _reader->GetSpaceMatches()) {
            if (match.path == prim) {
                *out = match.values;
                return true;
            }
        }
    }
    *error = "the rig publishes no " + path;
    return false;
}

bool
RigEvaluator::Reach(std::vector<ChannelReach> *reach) const
{
    reach->clear();
    rigExec::RigExecRuntimeInputReach always;
    if (!_reader->GetAlwaysReach(&always)) {
        return false;
    }
    const RigSetup &setup = *_setup;
    // Each output's index, by what the runtime calls it.
    std::map<std::string, std::vector<uint32_t>> meshByPoints, shaderByPath;
    for (size_t m = 0; m < setup.meshes.size(); ++m) {
        meshByPoints[setup.meshes[m].points].push_back(uint32_t(m));
    }
    for (size_t s = 0; s < setup.shaderMatrices.size(); ++s) {
        shaderByPath[setup.shaderMatrices[s]].push_back(uint32_t(s));
    }
    std::map<size_t, std::vector<uint32_t>> controlsBySlot, meshesBySlot;
    for (size_t c = 0; c < setup.controlPaths.size(); ++c) {
        const auto found = _frameSlots.find(setup.controlPaths[c]);
        if (found != _frameSlots.end()) {
            controlsBySlot[found->second].push_back(uint32_t(c));
        }
    }
    // A mesh placed by a provider moves with that provider's frame; one
    // whose provider has no frame is taken to move with anything.
    std::vector<uint32_t> meshesMovedByAll;
    for (size_t m = 0; m < setup.meshes.size(); ++m) {
        const std::string &provider = setup.meshes[m].provider;
        if (provider.empty()) {
            continue;
        }
        const auto found = _frameSlots.find(provider);
        if (found != _frameSlots.end()) {
            meshesBySlot[found->second].push_back(uint32_t(m));
        } else {
            meshesMovedByAll.push_back(uint32_t(m));
        }
    }
    const auto add = [](const std::vector<uint32_t> *found,
                        std::vector<uint32_t> *to) {
        if (found) {
            to->insert(to->end(), found->begin(), found->end());
        }
    };
    const auto find = [](const auto &map, const auto &key)
        -> const std::vector<uint32_t> * {
        const auto found = map.find(key);
        return found == map.end() ? nullptr : &found->second;
    };
    const auto tidy = [](std::vector<uint32_t> *v) {
        std::sort(v->begin(), v->end());
        v->erase(std::unique(v->begin(), v->end()), v->end());
    };
    reach->resize(setup.channelPaths.size());
    for (size_t i = 0; i < setup.channelPaths.size(); ++i) {
        if (_inputs[i] < 0) {
            continue;
        }
        rigExec::RigExecRuntimeInputReach input;
        if (!_reader->GetInputReach(size_t(_inputs[i]), &input)) {
            reach->clear();
            return false;
        }
        ChannelReach &out = (*reach)[i];
        for (const auto *part : {&input, &always}) {
            for (const std::string &path : part->points) {
                add(find(meshByPoints, path), &out.meshes);
            }
            for (const std::string &path : part->matrixPrimvars) {
                add(find(shaderByPath, path), &out.shaders);
            }
            for (const uint32_t slot : part->frameSlots) {
                add(find(controlsBySlot, size_t(slot)), &out.controls);
                add(find(meshesBySlot, size_t(slot)), &out.meshMatrices);
            }
        }
        out.meshMatrices.insert(out.meshMatrices.end(),
                                meshesMovedByAll.begin(),
                                meshesMovedByAll.end());
        tidy(&out.meshes);
        tidy(&out.meshMatrices);
        tidy(&out.controls);
        tidy(&out.shaders);
    }
    return true;
}

bool
RigEvaluator::_ControlFrame(const std::string &path, double matrix[16]) const
{
    const auto found = _frameSlots.find(path);
    if (found == _frameSlots.end()) {
        return false;
    }
    uint8_t role = 0;
    rigExec::RrPointFrame frame;
    if (!_reader->GetProviderFinalFrame(found->second, &role, &frame)) {
        return false;
    }
    FrameMatrix(frame, matrix);
    return true;
}

}  // namespace rigExecMaya
