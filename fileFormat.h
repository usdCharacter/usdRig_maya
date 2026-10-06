#pragma once
#include "pxr/pxr.h"
#include "pxr/usd/sdf/fileFormat.h"
PXR_NAMESPACE_OPEN_SCOPE
class UsdMayaRigFileFormat final : public SdfFileFormat {
public:
    bool CanRead(const std::string &) const override;
    bool Read(SdfLayer *, const std::string &, bool) const override;
    bool ReadFromString(SdfLayer *, const std::string &) const override;
    bool WriteToFile(const SdfLayer &, const std::string &, const std::string &,
                     const FileFormatArguments &) const override { return false; }
    bool WriteToString(const SdfLayer &, std::string *, const std::string &) const override;
    bool WriteToStream(const SdfSpecHandle &, std::ostream &, size_t) const override;
    std::set<std::string> GetExternalAssetDependencies(const SdfLayer &) const override;
protected:
    SDF_FILE_FORMAT_FACTORY_ACCESS;
    UsdMayaRigFileFormat();
    ~UsdMayaRigFileFormat() override = default;
private:
    bool _Read(SdfLayer *, const std::string &, const std::string &, bool) const;
};
PXR_NAMESPACE_CLOSE_SCOPE
