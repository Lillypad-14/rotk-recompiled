#include "bps_patch.h"

#include <algorithm>

#include "crc32.h"

namespace rotk {
namespace {

// BPS (byuu's beat patch format): "BPS1", source size, target size and metadata size as variable
// length numbers, the actions, then CRC32s of the source, the target and the patch.

struct Reader {
    const std::vector<std::uint8_t>& data;
    std::size_t at = 0;
    std::size_t end;

    bool number(std::uint64_t& out)
    {
        out = 0;
        std::uint64_t shift = 1;
        while (at < end) {
            const std::uint8_t byte = data[at++];
            out += (byte & 0x7Fu) * shift;
            if (byte & 0x80u) return true;
            shift <<= 7;
            out += shift;
        }
        return false;
    }
};

std::uint32_t le32(const std::vector<std::uint8_t>& data, std::size_t at)
{
    return static_cast<std::uint32_t>(data[at]) | static_cast<std::uint32_t>(data[at + 1]) << 8 |
           static_cast<std::uint32_t>(data[at + 2]) << 16 | static_cast<std::uint32_t>(data[at + 3]) << 24;
}

}  // namespace

bool bpsApply(const std::vector<std::uint8_t>& source, const std::vector<std::uint8_t>& patch,
              std::vector<std::uint8_t>& target, std::string* error)
{
    const auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (patch.size() < 4 + 12 || patch[0] != 'B' || patch[1] != 'P' || patch[2] != 'S' || patch[3] != '1')
        return fail("not a BPS patch");
    const std::size_t footer = patch.size() - 12;
    if (gba::crc32(patch.data(), patch.size() - 4) != le32(patch, footer + 8)) return fail("the patch is damaged");
    if (gba::crc32(source.data(), source.size()) != le32(patch, footer)) return fail("the ROM is not the one this patch is for");
    Reader in{patch, 4, footer};
    std::uint64_t sourceSize = 0, targetSize = 0, metadataSize = 0;
    if (!in.number(sourceSize) || !in.number(targetSize) || !in.number(metadataSize)) return fail("bad patch header");
    if (sourceSize != source.size()) return fail("the ROM is not the one this patch is for");
    if (targetSize > (1u << 26) || metadataSize > footer - in.at) return fail("bad patch header");
    in.at += metadataSize;
    target.assign(targetSize, 0);
    std::size_t out = 0, sourceRelative = 0, targetRelative = 0;
    while (in.at < footer) {
        std::uint64_t action = 0;
        if (!in.number(action)) return fail("bad patch action");
        const std::uint64_t length = (action >> 2) + 1;
        if (out + length > targetSize) return fail("the patch writes past the ROM");
        switch (action & 3) {
        case 0:  // SourceRead
            if (out + length > source.size()) return fail("the patch reads past the ROM");
            std::copy(source.begin() + out, source.begin() + out + length, target.begin() + out);
            out += length;
            break;
        case 1:  // TargetRead
            if (in.at + length > footer) return fail("the patch is truncated");
            std::copy(patch.begin() + in.at, patch.begin() + in.at + length, target.begin() + out);
            in.at += length;
            out += length;
            break;
        default: {  // SourceCopy, TargetCopy: a signed offset to the copy cursor
            std::uint64_t data = 0;
            if (!in.number(data)) return fail("bad patch action");
            const std::int64_t offset = (data & 1) ? -static_cast<std::int64_t>(data >> 1) : static_cast<std::int64_t>(data >> 1);
            std::size_t& cursor = (action & 3) == 2 ? sourceRelative : targetRelative;
            const std::int64_t from = static_cast<std::int64_t>(cursor) + offset;
            if (from < 0) return fail("bad patch copy");
            cursor = static_cast<std::size_t>(from);
            for (std::uint64_t i = 0; i < length; ++i) {
                if ((action & 3) == 2) {
                    if (cursor >= source.size()) return fail("the patch reads past the ROM");
                    target[out++] = source[cursor++];
                } else {
                    if (cursor >= out) return fail("bad patch copy");
                    target[out++] = target[cursor++];
                }
            }
            break;
        }
        }
    }
    if (out != targetSize) return fail("the patch ends early");
    if (gba::crc32(target.data(), target.size()) != le32(patch, footer + 4)) return fail("the patched ROM is wrong");
    return true;
}

}  // namespace rotk
