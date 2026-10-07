#pragma once
#include "modules/CpuSourcePreparation.h"
namespace corevideo::modules {
// Existing I420 arrival consumers share the same bounded multi-format owner.
using I420SourcePreparation = CpuSourcePreparation;
}
