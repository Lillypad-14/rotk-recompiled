// The only file compiled with the recomp-ui launcher flags. All the launcher logic is in
// GBARecomp's launcher_seam.h.

#include "launcher_seam.h"

#include "launcher_boot.h"

int rotk_launcher_preboot(std::vector<std::string>& args, const gbarecomp::RunOptions& options)
{
    return gbarecomp_launcher_preboot(args, options);
}
