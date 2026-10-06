#pragma once
#include "parser.h"
#include "pxr/usd/sdf/layer.h"
namespace usdMayaRig {
pxr::SdfLayerRefPtr Translate(Scene scene, const std::string &source, bool metadataOnly, bool strict);
}
