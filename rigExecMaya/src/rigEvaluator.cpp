#include "rigEvaluator.h"

#include "rigExecRuntime/runtime.h"

#include <cstring>

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
RigSetup::FrameSize() const
{
    return MeshMatricesOffset() + meshes.size() * 128;
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
    if (!evaluator->_reader->SetFrame(setup->bakeFrame, error)) {
        return nullptr;
    }
    // Which channels the bake takes, found once: a channel it refuses
    // would refuse every time.
    evaluator->_refused.assign(setup->channelPaths.size(), false);
    for (size_t i = 0; i < setup->channelPaths.size(); ++i) {
        std::string why;
        if (!evaluator->_reader->SetAvar(setup->channelPaths[i],
                                         setup->bakedValues[i], &why)) {
            evaluator->_refused[i] = true;
            evaluator->_refusedPaths.push_back(setup->channelPaths[i]);
        }
    }
    evaluator->_reader->ClearAvars();
    evaluator->_setup = std::move(setup);
    return evaluator;
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
    _reader->ClearAvars();
    for (size_t i = 0; i < values.size(); ++i) {
        if (!_refused[i] && values[i] != setup.bakedValues[i]) {
            _reader->SetAvar(setup.channelPaths[i], values[i], nullptr);
        }
    }
    for (const auto &[path, value] : overrides) {
        _reader->SetAvar(path, value, nullptr);
    }
    return _reader->Execute(error);
}

bool
RigEvaluator::Evaluate(const std::vector<double> &values,
                       std::vector<uint8_t> *frame, std::string *error)
{
    if (!_Run(values, {}, error)) {
        return false;
    }
    const RigSetup &setup = *_setup;
    frame->assign(setup.FrameSize(), 0);
    uint8_t *out = frame->data();

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

    for (const std::string &path : setup.controlPaths) {
        double matrix[16];
        if (!_reader->GetControlFrame(path, matrix)) {
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
        if (!_reader->GetControlFrame(path, matrix)) {
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

}  // namespace rigExecMaya
