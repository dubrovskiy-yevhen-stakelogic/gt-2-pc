#pragma once
// gt2tool commands of the course mod layer (docs/formats/track_json.md).
namespace gt2 {
class GtfsVolume;
}

namespace gt2tool {

// export-tracks <disc> <out-dir> [--course NAME] [--no-gltf]: every crsobj/*.tro -> <name>.json (+ glTF), each read back
// and resolved like gt2game --mods does, compared byte for byte with the disc's parsed data.
int CmdExportTracks(const gt2::GtfsVolume& vol, int argc, char** argv);
// import-track <disc> <track.json>: validates a course file, resolves it over its base course, prints what differs.
int CmdImportTrack(const gt2::GtfsVolume& vol, int argc, char** argv);

} // namespace gt2tool
