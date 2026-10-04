#include "App.h"
#include "Renderer.h"
#include "SmfExport.h"
#include "Fonts.h"
#include "Util.h"
#include "Widgets.h"

#include <GLFW/glfw3.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#endif

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace immidi {

const char* viewModeName(ViewMode m) {
    switch (m) {
    case ViewMode::Normal: return "Normal (all ports stacked)";
    case ViewMode::Compact: return "Compact rows";
    case ViewMode::PortTabs: return "One port at a time (tabs)";
    case ViewMode::SideBySide: return "Ports side by side";
    default: return "?";
    }
}

App::App(GLFWwindow* window) : window_(window) {
    std::fill(std::begin(vkKeyNote_), std::end(vkKeyNote_), -1);
    outputs_ = std::make_unique<MidiOutputs>();
    player_ = std::make_unique<Player>(*outputs_);
    snap_ = std::make_unique<PlayerSnapshot>();
    std::memset(heldNote_, 0xFF, sizeof heldNote_);
    infoReader_ = std::make_unique<SongInfoReader>([] { glfwPostEmptyEvent(); });  // wakes the idle main loop

    // Instrument definitions: next to the executable, in the working directory, and in the config dir.
    std::vector<std::string> dirs = {resourceDirectory() + "/insdef", "insdef", configDirectory() + "/insdef"};
    std::vector<std::string> loaded;
    for (const std::string& d : dirs) {
        std::error_code ec;
        std::string canon = pathToUtf8(std::filesystem::weakly_canonical(pathFromUtf8(d), ec));
        if (std::find(loaded.begin(), loaded.end(), canon) != loaded.end()) continue;
        if (ins_.loadDirectory(d) > 0) loaded.push_back(canon);
    }

    loadSettings();
    // Instrument conversion tables: the user's own copy, else the ones next to the program.
    conversionDir_ = ConversionTables::defaultFolder();
    refreshDevices();
    if (!soundBank_.empty()) {  // before the outputs open, so the synth starts with it
        outputs_->setSoftSynthBank(soundBank_);
        soundBankError_ = outputs_->softSynthBankError();
    }
    for (int p = 0; p < kMaxPorts; p++) {
        std::string dev = portDevice_[p];
        if (p == 0 && dev.empty()) {
            // First start: pick the first real output.
            for (const std::string& d : deviceList_)
                if (d != MidiOutputs::kNone && d != MidiOutputs::kVirtual) {
                    dev = d;
                    break;
                }
            portDevice_[0] = dev;
        }
        if (dev.empty() && p > 0) dev = portDevice_[0];
        // Names saved by older versions carry a port number; keep the name the device resolved to.
        if (outputs_->setPortDevice(p, dev) && !portDevice_[p].empty()) portDevice_[p] = outputs_->portDevice(p);
    }
    applyPlayerSettings();
    if (rememberPlaylist_) {
        playlist_.load(configDirectory() + "/playlist.m3u8");
        int cur = cfg_.getInt("playlist.current", -1);
        if (cur >= 0 && cur < int(playlist_.entries.size())) {
            playlist_.setCurrent(cur);
            loadIndex(cur, false);
        }
    }
    applyTheme();
}

App::~App() {
    infoReader_.reset();
    if (vkInput_) vkInput_->close();  // its thread calls into the player
    if (lcdList_) IM_DELETE(lcdList_);
    player_->stop();
    for (auto& j : loadJobs_)
        if (j->thread.joinable()) j->thread.join();
    saveSettings();
    player_.reset();
    outputs_.reset();
}

// ---------------------------------------------------------------- settings

void App::loadSettings() {
    cfg_.load(configDirectory() + "/settings.ini");
    songModules_.load(configDirectory() + "/song-modules.txt");
    savedSongModulesText_ = songModules_.serialize();
    for (int p = 0; p < kMaxPorts; p++) portDevice_[p] = cfg_.get("out.port" + std::to_string(p));
    mirrorEffects_ = cfg_.getBool("out.mirrorEffects", false);
    deviceSetting_ = cfg_.getInt("device", -1);
    fallbackDevice_ = cfg_.getInt("device.fallback", int(DeviceKind::SC88Pro));
    // Off by default: emulation changes the song data. (A new key, so settings saved while the old
    // default was "on" start with it off once.)
    emulation_ = cfg_.getBool("emulation.enabled", false);
    forcedToneMap_ = cfg_.getInt("gs.forceMap", 0);
    useConversionTables_ = cfg_.getBool("emulation.tables", true);
    resetMode_ = cfg_.getInt("reset", int(ResetMode::Auto));
    resetDelayMs_ = cfg_.getInt("reset.delay", 200);
    resetOnSeek_ = cfg_.getBool("reset.onSeek", false);
    loopEnabled_ = cfg_.getBool("loop", true);
    loopRepeats_ = cfg_.getInt("loop.repeats", 1);
    encodingOverride_ = cfg_.getInt("encoding", 0);
    infoReader_->setEncoding(TextEncoding(encodingOverride_));
    kbLo_ = cfg_.getInt("keyboard.lo", 12);
    kbHi_ = cfg_.getInt("keyboard.hi", 119);
    viewMode_ = ViewMode(std::clamp(cfg_.getInt("view", 0), 0, int(ViewMode::Count) - 1));
    autoCompactMultiport_ = cfg_.getBool("view.autoCompact", true);
    showPlaylistPane_ = cfg_.getBool("view.playlistPane", true);
    showLcd_ = cfg_.getBool("view.lcd", false);
    showVk_ = cfg_.getBool("view.keyboard", false);
    if (cfg_.getBool("view.mini", false)) setMiniMode(true);
    vkPort_ = std::clamp(cfg_.getInt("vk.port", 0), 0, kMaxPorts - 1);
    vkCh_ = std::clamp(cfg_.getInt("vk.channel", 0), 0, 15);
    vkVelocity_ = std::clamp(cfg_.getInt("vk.velocity", 100), 1, 127);
    vkOctave_ = std::clamp(cfg_.getInt("vk.octave", 3), -1, 8);
    vkMouseVelocityByPos_ = cfg_.getBool("vk.mouseVelocityByPosition", false);
    vkRemap_ = cfg_.getBool("vk.inputToChannel", true);
    vkInputName_ = cfg_.get("vk.input");
    soundBank_ = cfg_.get("dls.bank");
    rendererSetting_ = cfg_.get("gfx.renderer");
    lcdSize_ = std::clamp(cfg_.getInt("view.lcdSize", 2), 0, 2);
    loadLcdColors();
    showChannelDetail_ = cfg_.getBool("view.channelDetail", true);
    uiScale_ = float(cfg_.getDouble("ui.scale", 1.0));
    darkTheme_ = cfg_.getBool("ui.dark", true);
    meterDecay_ = float(cfg_.getDouble("ui.meterDecay", 1.6));
    speedPercent_ = cfg_.getInt("speed", 100);
    transpose_ = cfg_.getInt("transpose", 0);
    masterVolume_ = cfg_.getInt("volume", 127);
    playlist_.mode = PlayMode(std::clamp(cfg_.getInt("playMode", 0), 0, int(PlayMode::Count) - 1));
    for (int d = 0; d < int(DeviceKind::Count); d++) {
        insOverrideMelodic_[d] = cfg_.get("ins.melodic." + std::to_string(d));
        insOverrideDrum_[d] = cfg_.get("ins.drums." + std::to_string(d));
    }
    lastDir_ = cfg_.get("lastDir");
    rememberPlaylist_ = cfg_.getBool("playlist.remember", true);
    playDroppedFiles_ = cfg_.getBool("playlist.playDropped", true);
    lyricsFontScale_ = float(cfg_.getDouble("lyrics.scale", 2.0));
    dropLateNotes_ = cfg_.getBool("perf.dropLateNotes", true);
    useNativeDialogs_ = cfg_.getBool("dialogs.native", true);
    addSubfolders_ = cfg_.getBool("dialogs.addSubfolders", true);
    exportRelative_ = cfg_.getBool("playlist.exportRelative", true);
    maxLagMs_ = cfg_.getInt("perf.maxLagMs", 50);
    minVelocity_ = cfg_.getInt("perf.minVelocity", 0);
}

void App::saveSettings() {
    storeSettingsToConfig();
    std::string text = cfg_.serialize();
    if (text != savedSettingsText_ && cfg_.save(configDirectory() + "/settings.ini")) savedSettingsText_ = text;
    std::string modules = songModules_.serialize();
    if (modules != savedSongModulesText_ && songModules_.save(configDirectory() + "/song-modules.txt")) savedSongModulesText_ = modules;
    if (rememberPlaylist_) {
        std::string pl = playlist_.serialize();
        if (pl != savedPlaylistText_ && playlist_.save(configDirectory() + "/playlist.m3u8")) savedPlaylistText_ = pl;
    }
}

void App::saveIfChanged() {
    // No timer: a save is considered only when something may have changed, i.e. a user
    // interaction just ended, the playlist was edited, or code asked for it (file loaded).
    // saveSettings() then writes only the files whose content actually differs.
    bool interactionEnded = false;
    for (int b = 0; b < ImGuiMouseButton_COUNT && !interactionEnded; b++) interactionEnded = ImGui::IsMouseReleased(b);
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END && !interactionEnded; k++)
        interactionEnded = ImGui::IsKeyReleased(ImGuiKey(k));
    if (!interactionEnded && !saveRequested_ && playlist_.revision == savedPlaylistRev_) return;
    saveRequested_ = false;
    savedPlaylistRev_ = playlist_.revision;
    saveSettings();
}

void App::storeSettingsToConfig() {
    for (int p = 0; p < kMaxPorts; p++) cfg_.set("out.port" + std::to_string(p), portDevice_[p]);
    cfg_.setBool("out.mirrorEffects", mirrorEffects_);
    cfg_.setInt("device", deviceSetting_);
    cfg_.setInt("device.fallback", fallbackDevice_);
    cfg_.setBool("emulation.enabled", emulation_);
    cfg_.setInt("gs.forceMap", forcedToneMap_);
    cfg_.setBool("emulation.tables", useConversionTables_);
    cfg_.setInt("reset", resetMode_);
    cfg_.setInt("reset.delay", resetDelayMs_);
    cfg_.setBool("reset.onSeek", resetOnSeek_);
    cfg_.setBool("loop", loopEnabled_);
    cfg_.setInt("loop.repeats", loopRepeats_);
    cfg_.setInt("encoding", encodingOverride_);
    cfg_.setInt("keyboard.lo", kbLo_);
    cfg_.setInt("keyboard.hi", kbHi_);
    cfg_.setInt("view", int(viewMode_));
    cfg_.setBool("view.autoCompact", autoCompactMultiport_);
    cfg_.setBool("view.playlistPane", showPlaylistPane_);
    cfg_.setBool("view.lcd", showLcd_);
    cfg_.setBool("view.keyboard", showVk_);
    cfg_.setBool("view.mini", miniMode_);
    cfg_.setInt("vk.port", vkPort_);
    cfg_.setInt("vk.channel", vkCh_);
    cfg_.setInt("vk.velocity", vkVelocity_);
    cfg_.setInt("vk.octave", vkOctave_);
    cfg_.setBool("vk.mouseVelocityByPosition", vkMouseVelocityByPos_);
    cfg_.setBool("vk.inputToChannel", vkRemap_);
    cfg_.set("vk.input", vkInputName_);
    cfg_.set("dls.bank", soundBank_);
    cfg_.set("gfx.renderer", rendererSetting_);
    cfg_.setInt("view.lcdSize", lcdSize_);
    storeLcdColors();
    cfg_.setBool("view.channelDetail", showChannelDetail_);
    cfg_.setDouble("ui.scale", uiScale_);
    cfg_.setBool("ui.dark", darkTheme_);
    cfg_.setDouble("ui.meterDecay", meterDecay_);
    cfg_.setInt("speed", speedPercent_);
    cfg_.setInt("transpose", transpose_);
    cfg_.setInt("volume", masterVolume_);
    cfg_.setInt("playMode", int(playlist_.mode));
    for (int d = 0; d < int(DeviceKind::Count); d++) {
        cfg_.set("ins.melodic." + std::to_string(d), insOverrideMelodic_[d]);
        cfg_.set("ins.drums." + std::to_string(d), insOverrideDrum_[d]);
    }
    cfg_.set("lastDir", lastDir_);
    cfg_.setBool("playlist.remember", rememberPlaylist_);
    cfg_.setBool("playlist.playDropped", playDroppedFiles_);
    cfg_.setInt("playlist.current", playlist_.current);
    cfg_.setDouble("lyrics.scale", lyricsFontScale_);
    cfg_.setBool("perf.dropLateNotes", dropLateNotes_);
    cfg_.setBool("dialogs.native", useNativeDialogs_);
    cfg_.setBool("dialogs.addSubfolders", addSubfolders_);
    cfg_.setBool("playlist.exportRelative", exportRelative_);
    cfg_.setInt("perf.maxLagMs", maxLagMs_);
    cfg_.setInt("perf.minVelocity", minVelocity_);
}

void App::applyPlayerSettings() {
    player_->setDevice(DeviceKind(deviceSetting_ < 0 ? fallbackDevice_ : deviceSetting_), deviceSetting_ < 0);
    player_->setEmulation(emulation_);
    player_->setForcedToneMap(forcedToneMap_);
    player_->setConversionTables(useConversionTables_, conversionDir_);
    player_->setResetMode(ResetMode(resetMode_));
    player_->setResetDelayMs(resetDelayMs_);
    player_->setResetOnSeek(resetOnSeek_);
    player_->setLoop(loopEnabled_, loopRepeats_);
    player_->setSpeed(speedPercent_ / 100.0);
    player_->setTranspose(transpose_);
    player_->setNoteSkipping(dropLateNotes_ ? maxLagMs_ : 0, minVelocity_);
    player_->setMirrorEffects(mirrorEffects_);
}

void App::setDevice(int deviceIndex) {
    deviceSetting_ = deviceIndex;
    applyPlayerSettings();
}

void App::setOutputForAllPorts(const std::string& device) {
    for (int p = 0; p < kMaxPorts; p++) {
        portDevice_[p] = device;
        if (outputs_->setPortDevice(p, device)) portDevice_[p] = outputs_->portDevice(p);
    }
}

void App::refreshDevices() { deviceList_ = outputs_->availableDevices(); }

void App::applyTheme() {
    if (darkTheme_) ImGui::StyleColorsDark();
    else ImGui::StyleColorsLight();
    ImGuiStyle& s = ImGui::GetStyle();
    s.FrameRounding = 3.0f;
    s.GrabRounding = 3.0f;
    s.WindowRounding = 0.0f;
    s.TabRounding = 3.0f;
    s.ItemSpacing = ImVec2(6, 4);
    s.FramePadding = ImVec2(6, 3);
    s.CellPadding = ImVec2(3, 2);
    if (darkTheme_) {
        s.Colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.12f, 1.0f);
        s.Colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.0f);
        s.Colors[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.19f, 0.23f, 1.0f);
        s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.25f, 0.31f, 1.0f);
        s.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    }
    s.FontScaleMain = uiScale_;
}

// ---------------------------------------------------------------- files / playlist

void App::openPaths(const std::vector<std::string>& paths, bool replacePlaylist, bool play, int insertAt) {
    if (paths.empty()) return;
    if (replacePlaylist) {
        player_->stop();
        playlist_.clear();
    }
    int firstNew = int(playlist_.entries.size());
    for (const std::string& given : paths) {
        // Absolute, so the playlist (and what is remembered per song) still finds the files when
        // ImMidi starts from another folder (relative paths come from the command line).
        std::error_code ec;
        std::filesystem::path abs = std::filesystem::absolute(pathFromUtf8(given), ec);
        const std::string p = ec ? given : pathToUtf8(abs.lexically_normal());
        if (std::filesystem::is_directory(pathFromUtf8(p), ec)) playlist_.addDirectory(p, true);
        else if (endsWithNoCase(p, ".m3u") || endsWithNoCase(p, ".m3u8")) {
            Playlist tmp;
            if (tmp.load(p))
                for (auto& e : tmp.entries) playlist_.add(std::move(e));
        } else playlist_.add(p);
        lastDir_ = pathToUtf8(pathFromUtf8(p).parent_path());
    }
    if (firstNew >= int(playlist_.entries.size())) return;
    if (insertAt >= 0 && insertAt < firstNew) {
        playlist_.placeAddedAt(firstNew, insertAt);
        firstNew = insertAt;
    }
    if (play || replacePlaylist || playlist_.current < 0) loadIndex(firstNew, play);
}

void App::dropPaths(const std::vector<std::string>& paths, float x, float y) {
    // The drop callback runs between frames, so the geometry is from the frame just shown.
    ImVec2 p(x, y);
    for (const PlaylistGeometry& g : plGeom_) {
        if (g.frame < ImGui::GetFrameCount() - 1) continue;  // not on screen
        if (p.x < g.areaMin.x || p.x >= g.areaMax.x || p.y < g.areaMin.y || p.y >= g.areaMax.y) continue;
        openPaths(paths, false, false, playlistInsertIndex(g, p));  // -1 (appended) over the buttons or status line
        return;
    }
    openPaths(paths, false, playDroppedFiles_);
}

void App::loadIndex(int index, bool play, int skipTries) {
    if (index < 0 || index >= int(playlist_.entries.size())) return;
    playlist_.setCurrent(index);
    if (play) player_->stop();
    auto job = std::make_unique<LoadJob>();
    job->index = index;
    job->play = play;
    job->skipTries = skipTries;
    job->path = playlist_.entries[size_t(index)].path;
    LoadJob* j = job.get();
    int enc = encodingOverride_;
    job->thread = std::thread([j, enc] {
        auto mf = std::make_shared<MidiFile>();
        std::string err;
        if (mf->load(j->path, err, &j->progress)) {
            if (enc != 0) mf->setEncoding(TextEncoding(enc));
            j->info = analyzeFile(*mf);
            j->file = mf;
        } else {
            j->error = err;
        }
        j->done = true;
        glfwPostEmptyEvent();  // wake the main loop if it is idling
    });
    loadJobs_.push_back(std::move(job));
}

void App::pollLoading() {
    for (size_t i = 0; i < loadJobs_.size();) {
        LoadJob& j = *loadJobs_[i];
        if (!j.done) {
            i++;
            continue;
        }
        if (j.thread.joinable()) j.thread.join();
        bool current = (i + 1 == loadJobs_.size());
        if (current) {
            // The entry may have moved while loading (sorted, dragged): the current entry follows it.
            int n = int(playlist_.entries.size()), index = j.index;
            if (index >= n || playlist_.entries[size_t(index)].path != j.path)
                index = playlist_.current >= 0 && playlist_.current < n && playlist_.entries[size_t(playlist_.current)].path == j.path ? playlist_.current : -1;
            if (j.file && index >= 0) {
                applyLoadedFile(index, j.file, j.info, j.play);
            } else if (!j.file) {
                loadError_ = j.path + ": " + j.error;
                if (index >= 0) {
                    PlaylistEntry& e = playlist_.entries[size_t(index)];
                    e.infoState = InfoState::Unreadable;
                    e.infoError = j.error;
                    playlist_.revision++;
                }
                // While auto-advancing, skip unreadable files (at most once around the playlist).
                if (j.play && j.skipTries + 1 < int(playlist_.entries.size())) {
                    int next = playlist_.nextManual();
                    int tries = j.skipTries + 1;
                    loadJobs_.erase(loadJobs_.begin() + long(i));
                    loadIndex(next, true, tries);
                    return;
                }
            }
        }
        loadJobs_.erase(loadJobs_.begin() + long(i));
    }
}

void App::applyLoadedFile(int index, std::shared_ptr<MidiFile> mf, const FileInfo& info, bool play) {
    PlaylistEntry& e = playlist_.entries[size_t(index)];
    loadError_.clear();
    e.info = songInfoOf(*mf);  // opening a file refreshes its details
    e.infoState = InfoState::Known;
    e.infoError.clear();
    playlist_.revision++;
    detectedInfo_ = info;
    info_ = songInfoWithModule(e.path, info);
    player_->setFile(mf, info_);
    outputs_->preloadSoftSynth(*mf);  // instruments of a custom sound bank (macOS), before playback
    file_ = mf;
    fileMut_ = mf;
    std::memset(meter_, 0, sizeof meter_);
    selPort_ = std::min(selPort_, mf->numPorts - 1);
    if (pendingSeekUs_ >= 0) {
        player_->seek(pendingSeekUs_);
        pendingSeekUs_ = -1;
    }
    if (play) player_->play();
}

FileInfo App::songInfoWithModule(const std::string& path, const FileInfo& detected) const {
    DeviceKind d;
    return songModules_.get(path, d) ? withSongModule(detected, d) : detected;
}

void App::setSongModule(const std::vector<std::string>& paths, int device) {
    for (const std::string& p : paths) {
        if (device < 0) songModules_.clear(p);
        else songModules_.set(p, DeviceKind(device));
    }
    if (file_ && std::find(paths.begin(), paths.end(), file_->path) != paths.end()) {
        info_ = songInfoWithModule(file_->path, detectedInfo_);
        player_->setFileInfo(info_);  // the conversion follows at once, also while playing
    }
    saveRequested_ = true;
}

void App::songModuleMenu(const std::vector<std::string>& paths) {
    // What the songs share: -1 = none set, -2 = they differ.
    int shared = -3;
    for (const std::string& p : paths) {
        DeviceKind d;
        int v = songModules_.get(p, d) ? int(d) : -1;
        shared = shared == -3 ? v : shared == v ? v : -2;
    }
    std::string detected = "As detected";
    if (file_ && paths.size() == 1 && paths[0] == file_->path)
        detected += std::string(" (") + deviceProfile(detectedInfo_.suggestedDevice).name + ")";
    if (ImGui::MenuItem(detected.c_str(), nullptr, shared == -1)) setSongModule(paths, -1);
    ImGui::SetItemTooltip("Go by the song's own messages and text");
    ImGui::Separator();
    for (int d = 0; d < deviceCount(); d++)
        if (ImGui::MenuItem(deviceProfile(DeviceKind(d)).name, nullptr, shared == d)) setSongModule(paths, d);
}

void App::pollSongInfo() {
    // Results first: an entry whose details just arrived is not queued again.
    std::vector<SongInfoReader::Result> results = infoReader_->takeResults();
    if (!results.empty()) {
        std::unordered_map<std::string, const SongInfoReader::Result*> byPath;
        for (const SongInfoReader::Result& r : results) byPath[r.path] = &r;
        for (PlaylistEntry& e : playlist_.entries) {
            if (e.infoState != InfoState::Unknown) continue;  // e.g. opened meanwhile: that is newer
            auto it = byPath.find(e.path);
            if (it == byPath.end()) continue;
            e.info = it->second->info;
            e.infoState = it->second->ok ? InfoState::Known : InfoState::Unreadable;
            e.infoError = it->second->error;
        }
        infoArrived_ = true;
    }
    if (playlist_.revision != infoQueuedRev_) {
        infoQueuedRev_ = playlist_.revision;
        std::vector<std::string> unread;
        std::unordered_set<std::string> seen;  // a file listed twice is read once
        for (const PlaylistEntry& e : playlist_.entries)
            if (e.infoState == InfoState::Unknown && seen.insert(e.path).second) unread.push_back(e.path);
        infoReader_->setQueue(std::move(unread));
    }
    // Saved once a batch is done, not after every file.
    if (infoArrived_ && infoReader_->pending() == 0) {
        infoArrived_ = false;
        playlist_.revision++;
        infoQueuedRev_ = playlist_.revision;  // nothing new to queue
    }
}

void App::playNext(bool manual) {
    int next = manual ? playlist_.nextManual() : playlist_.nextAfterFinish();
    if (next < 0) {
        player_->stop();
        return;
    }
    loadIndex(next, true);
}

void App::playPrevious() {
    if (player_->positionUs() > 3000000) {
        player_->seek(0);
        return;
    }
    int prev = playlist_.previousManual();
    if (prev >= 0) loadIndex(prev, true);
}

std::string App::windowTitle() const {
    if (!file_) return "ImMidi";
    std::string t = file_->title.empty() ? file_->fileName : file_->title + " (" + file_->fileName + ")";
    return t + " - ImMidi";
}

// ---------------------------------------------------------------- instruments

const DeviceProfile& App::profile() const { return deviceProfile(snap_->device); }

// Finds the first available definition of a ';' separated list of names.
static const InsInstrument* findFirstIns(const InsLibrary& lib, const std::string& names) {
    size_t pos = 0;
    while (pos <= names.size()) {
        size_t sep = names.find(';', pos);
        if (sep == std::string::npos) sep = names.size();
        if (const InsInstrument* i = lib.find(names.substr(pos, sep - pos))) return i;
        pos = sep + 1;
    }
    return nullptr;
}

void App::resolveInsDefs() const {
    int frame = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -2;
    if (frame == insDefsFrame_ && frame >= 0) return;
    insDefsFrame_ = frame;
    const DeviceProfile& p = profile();
    const std::string& om = insOverrideMelodic_[int(p.kind)];
    const InsInstrument* m = om.empty() ? nullptr : ins_.find(om);
    melodicInsCache_ = m ? m : findFirstIns(ins_, p.insMelodic);
    const std::string& od = insOverrideDrum_[int(p.kind)];
    const InsInstrument* d = od.empty() ? nullptr : ins_.find(od);
    drumInsCache_ = d ? d : findFirstIns(ins_, p.insDrums);
}

const InsInstrument* App::melodicIns() const {
    resolveInsDefs();
    return melodicInsCache_;
}

const InsInstrument* App::drumIns() const {
    resolveInsDefs();
    return drumInsCache_;
}

int App::insBank(const ChannelState& c, bool drum) const {
    const DeviceProfile& p = profile();
    switch (p.family) {
    case MidiStandard::GS: {
        int map = c.bankLsb;
        if (c.gsToneMap > 0) map = c.gsToneMap;
        if (map == 0 && c.gsToneMap0 > 0) map = c.gsToneMap0;
        if (map == 0 && snap_->forcedToneMap > 0) map = snap_->forcedToneMap;
        if (map == 0) {
            if (p.kind == DeviceKind::SC55) map = 1;
            else if (p.kind == DeviceKind::SC88) map = 2;
        }
        if (map > p.gsMaxMap && p.gsMaxMap > 0) map = 0;
        return drum ? map : c.bankMsb * 128 + map;
    }
    case MidiStandard::XG:
        return drum ? c.bankMsb * 128 : c.bankMsb * 128 + c.bankLsb;
    case MidiStandard::GM2:
        return drum ? 120 * 128 : 121 * 128 + (c.bankMsb == 121 ? c.bankLsb : 0);
    default:
        return 0;
    }
}

std::string App::instrumentName(int port, int ch, bool* fallback) const {
    const ChannelState& c = snap_->ports[port].ch[ch];
    if (fallback) *fallback = false;
    const InsInstrument* ins = c.drum ? drumIns() : melodicIns();
    int bank = insBank(c, c.drum);
    NameCacheEntry& cache = nameCache_[port & (kMaxPorts - 1)][ch & 15];
    if (cache.valid && cache.ins == ins && cache.bank == bank && cache.program == c.program && cache.drum == c.drum) {
        if (fallback) *fallback = cache.fallback;
        return cache.name;
    }
    bool fb = false;
    std::string name = instrumentNameUncached(c, ins, bank, &fb);
    cache = NameCacheEntry{ins, bank, c.program, c.drum, fb, true, name};
    if (fallback) *fallback = fb;
    return name;
}

std::string App::instrumentNameUncached(const ChannelState& c, const InsInstrument* ins, int bank, bool* fallback) const {
    std::string name = InsLibrary::patchName(ins, bank, c.program);
    if (name.empty() && !c.drum) {
        // Variation missing: GS and XG modules fall back to the capital tone.
        int base = profile().family == MidiStandard::GS ? (bank & 127) : (profile().family == MidiStandard::GM2 ? 121 * 128 : 0);
        name = InsLibrary::patchName(ins, base, c.program);
        if (name.empty()) name = InsLibrary::patchName(ins_.find("General MIDI"), 0, c.program);
        if (!name.empty() && fallback) *fallback = true;
    }
    if (name.empty() && c.drum) {
        name = InsLibrary::patchName(ins, profile().family == MidiStandard::GS ? 0 : bank, c.program);
        if (name.empty()) name = "Drum Kit " + std::to_string(c.program + 1);
        else if (fallback) *fallback = true;
    }
    if (name.empty()) name = "Program " + std::to_string(c.program + 1);
    return name;
}

std::string App::drumNoteName(int port, int ch, int note) const {
    const ChannelState& c = snap_->ports[port].ch[ch];
    // A kit with its own note list names only the keys that sound (e.g. XG SFX kits); General MIDI
    // names are for kits the definitions do not describe.
    if (const NameList* l = InsLibrary::noteList(drumIns(), insBank(c, true), c.program)) {
        auto it = l->find(note);
        return it != l->end() ? it->second : std::string();
    }
    return InsLibrary::noteName(ins_.find("General MIDI Drums"), 0, 0, note);
}

// ---------------------------------------------------------------- frame

void App::updateMeters(float dt) {
    for (int p = 0; p < snap_->numPorts; p++)
        for (int c = 0; c < 16; c++) {
            const ChannelState& s = snap_->ports[p].ch[c];
            float& m = meter_[p][c];
            m = std::max(0.0f, m - meterDecay_ * dt);
            if (s.noteOnSerial != lastSerial_[p][c]) {
                lastSerial_[p][c] = s.noteOnSerial;
                float v = s.lastOnVelocity / 127.0f * s.cc[7] / 127.0f * s.cc[11] / 127.0f;
                m = std::max(m, std::sqrt(v));
            }
        }
}

void App::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    // The playlist keeps the keyboard only while it is on screen.
    if (plFocusView_ >= 0 && plGeom_[plFocusView_].frame < ImGui::GetFrameCount() - 1) plFocusView_ = -1;
    if (io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false)) {
        showVk_ = !showVk_;
        if (showVk_) setMiniMode(false);  // the keyboard window needs the room
    }
    // Ctrl+M would be Cmd+M on macOS, which minimizes the window.
    if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_M, false)) setMiniMode(!miniMode_);
    if (showVk_ && vkFocused_) {
        // The virtual keyboard plays these keys; only panic stays global.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) player_->panic();
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) player_->togglePlayPause();
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) player_->seek(std::max<int64_t>(0, player_->positionUs() - 5000000));
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) player_->seek(player_->positionUs() + 5000000);
    // Page Up/Down move through the playlist view while it has the keyboard.
    bool pageKeys = plFocusView_ < 0;
    if ((pageKeys && ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) || ImGui::IsKeyPressed(ImGuiKey_N, false)) playNext(true);
    if ((pageKeys && ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) || ImGui::IsKeyPressed(ImGuiKey_B, false)) playPrevious();
    if (ImGui::IsKeyPressed(ImGuiKey_Period, false)) player_->stop();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) player_->panic();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) showFileDialog(DialogPurpose::OpenFiles);
    if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) {
        transpose_ = std::min(24, transpose_ + 1);
        player_->setTranspose(transpose_);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) {
        transpose_ = std::max(-24, transpose_ - 1);
        player_->setTranspose(transpose_);
    }
    for (int k = 0; k < 7; k++)
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_1 + k), false)) requestedTab_ = Tab(k);
}

void App::frame() {
    float dt = ImGui::GetIO().DeltaTime;
    pollLoading();
    pollSongInfo();
    player_->snapshot(*snap_);
    updateMeters(dt);
    if (player_->consumeFinished()) playNext(false);
    handleShortcuts();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_MenuBar |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;
    const ImVec2 padding(8, 6);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::Begin("ImMidi", nullptr, flags);
    ImGui::PopStyleVar();
    drawMenuBar();
    drawTransport();
    drawInfoBar();
    // A tab was asked for (menu, Ctrl+1..7), or the built-in file browser is open: both need the room.
    if (miniMode_ && (requestedTab_ != Tab::Count || browser_.isOpen())) setMiniMode(false);
    if (miniMode_) {
        fitMiniWindow(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + padding.y);
        // The virtual keyboard is not drawn: it has no focus, and notes held with it end (its MIDI
        // input stays open).
        if (vkFocused_ || vkMouseNote_ >= 0) vkReleaseAll();
        vkFocused_ = false;
        std::vector<std::string> picked;
        if (nativeDialogs_.poll(picked)) handleDialogResult(dialogPurpose_, picked, addSubfolders_);
        drawAbout();
        drawFileInfo();
        ImGui::End();
        saveIfChanged();
        return;
    }

    float paneW = (showPlaylistPane_ && ImGui::GetContentRegionAvail().x > 1150) ? 330.0f * uiScale_ : 0.0f;
    ImGui::BeginChild("##main", ImVec2(paneW > 0 ? -paneW - 6 : 0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    if (ImGui::BeginTabBar("##tabs")) {
        auto tabItem = [&](Tab t, const char* label) {
            ImGuiTabItemFlags f = (requestedTab_ == t) ? ImGuiTabItemFlags_SetSelected : 0;
            bool open = ImGui::BeginTabItem(label, nullptr, f);
            if (open) tab_ = t;
            return open;
        };
        if (tabItem(Tab::Channels, "Channels")) {
            drawChannels();
            ImGui::EndTabItem();
        }
        if (tabItem(Tab::SoundEdit, "Sound Edit")) {
            drawSoundEdit();
            ImGui::EndTabItem();
        }
        if (tabItem(Tab::Effects, "Effects")) {
            drawEffects();
            ImGui::EndTabItem();
        }
        if (tabItem(Tab::Lyrics, "Lyrics")) {
            drawLyrics();
            ImGui::EndTabItem();
        }
        if (tabItem(Tab::Playlist, "Playlist")) {
            drawPlaylist(false);
            ImGui::EndTabItem();
        }
        if (tabItem(Tab::Info, "File Info")) {
            drawInfo();
            ImGui::EndTabItem();
        }
        if (tabItem(Tab::Settings, "Settings")) {
            drawSettings();
            ImGui::EndTabItem();
        }
        requestedTab_ = Tab::Count;
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    if (paneW > 0) {
        ImGui::SameLine();
        ImGui::BeginChild("##playlistPane", ImVec2(0, 0), ImGuiChildFlags_Borders);
        drawPlaylist(true);
        ImGui::EndChild();
    }

    std::vector<std::string> picked;
    if (nativeDialogs_.poll(picked)) handleDialogResult(dialogPurpose_, picked, addSubfolders_);
    if (browser_.draw()) {
        lastDir_ = browser_.currentDir();
        handleDialogResult(dialogPurpose_, browser_.selection(), browser_.recursive());
    }
    drawInstrumentPicker();
    drawLcdWindow();
    drawVirtualKeyboard();
    drawAbout();
    drawFileInfo();
    ImGui::End();
    saveIfChanged();
}

// ---------------------------------------------------------------- file dialogs

void App::showFileDialog(DialogPurpose purpose) {
    if (nativeDialogs_.busy() || browser_.isOpen()) return;
    const std::vector<std::string> midiExt = {"mid", "midi", "kar", "rmi", "smf", "mds", "rcp", "xmi"};
    const std::vector<std::string> listExt = {"m3u", "m3u8"};
    DialogRequest req;
    req.startDir = lastDir_;
    switch (purpose) {
    case DialogPurpose::OpenFiles:
    case DialogPurpose::AddFiles: {
        req.kind = DialogKind::OpenFiles;
        req.title = purpose == DialogPurpose::OpenFiles ? "Open MIDI files" : "Add files to the playlist";
        std::vector<std::string> all = midiExt;
        all.insert(all.end(), listExt.begin(), listExt.end());
        req.filters = {{"MIDI files and playlists", all}, {"MIDI files", midiExt}, {"Playlists", listExt}};
        break;
    }
    case DialogPurpose::AddFolder:
        req.kind = DialogKind::SelectFolder;
        req.title = "Add a folder to the playlist";
        break;
    case DialogPurpose::ImportPlaylist:
    case DialogPurpose::AppendPlaylist:
        req.kind = DialogKind::OpenFile;
        req.title = purpose == DialogPurpose::ImportPlaylist ? "Import playlist" : "Append playlist";
        req.filters = {{"Playlists (.m3u, .m3u8)", listExt}};
        break;
    case DialogPurpose::ExportPlaylist:
        req.kind = DialogKind::SaveFile;
        req.title = "Export playlist";
        req.defaultName = "playlist.m3u";
        req.filters = {{"M3U playlist", {"m3u"}}, {"M3U8 playlist (UTF-8)", {"m3u8"}}};
        break;
    case DialogPurpose::ExportSmf:
    case DialogPurpose::ExportSmfEmulated: {
        if (!file_) return;
        std::filesystem::path src = pathFromUtf8(file_->path);
        std::string stem = pathToUtf8(src.stem());
        bool isMid = endsWithNoCase(file_->path, ".mid") || endsWithNoCase(file_->path, ".midi");
        req.kind = DialogKind::SaveFile;
        if (purpose == DialogPurpose::ExportSmf) {
            req.title = "Export as Standard MIDI File";
            req.defaultName = stem + (isMid ? "-export.mid" : ".mid");
        } else {
            DeviceKind target = player_->device();
            req.title = std::string("Export with emulation for ") + deviceShortName(target);
            req.defaultName = stem + "-" + deviceId(target) + ".mid";
        }
        req.startDir = pathToUtf8(src.parent_path());
        req.filters = {{"Standard MIDI File", {"mid"}}};
        break;
    }
    case DialogPurpose::SoundBank:
        req.kind = DialogKind::OpenFiles;
        req.title = "Choose a sound bank for the Apple DLS Synth";
        if (!soundBank_.empty()) req.startDir = pathToUtf8(pathFromUtf8(soundBank_).parent_path());
        req.filters = {{"Sound banks (.sf2, .dls)", {"sf2", "dls"}}};
        break;
    default: return;
    }
    dialogPurpose_ = purpose;
    void* parent = nullptr;
#if defined(_WIN32)
    parent = glfwGetWin32Window(window_);
#elif defined(__APPLE__)
    parent = (void*)glfwGetCocoaWindow(window_);
#endif
    if (useNativeDialogs_ && NativeDialogs::available() && nativeDialogs_.start(req, parent)) return;
    // Built-in fallback
    std::vector<std::string> exts;
    for (const DialogFilter& f : req.filters)
        for (const std::string& e : f.extensions) exts.push_back("." + e);
    setMiniMode(false);  // the built-in browser needs the room
    FileBrowser::Mode mode = req.kind == DialogKind::SelectFolder ? FileBrowser::Mode::SelectFolder
                             : req.kind == DialogKind::SaveFile   ? FileBrowser::Mode::SaveFile
                                                                  : FileBrowser::Mode::OpenFiles;
    browser_.open(mode, req.title, req.startDir, exts, req.defaultName);
}

bool App::needsContinuousFrames() const {
    if (snap_->playing || snap_->starting || !loadJobs_.empty() || extDragInside_) return true;
    const bool lcd = showLcd_ && !miniMode_;
    if ((tab_ == Tab::Channels && !miniMode_) || lcd)  // where level meters are shown
        for (int p = 0; p < snap_->numPorts; p++)
            for (int c = 0; c < 16; c++)
                if (meter_[p][c] > 0.0f) return true;  // still falling
    double now = ImGui::GetTime();
    if (lcd && (now < lcdDotsUntil_ || lcdTextStart_ >= 0)) return true;
    if (showVk_ && !miniMode_ && vkInput_ && vkInput_->isOpen()) {
        // Let the activity light go out after the last message.
        static uint32_t seen = 0;
        static double until = 0;
        uint32_t a = vkInActivity_.load();
        if (a != seen) {
            seen = a;
            until = now + 0.5;
        }
        if (now < until) return true;
    }
    return false;
}

double App::frameInterval() const {
    if (!needsContinuousFrames()) return -1.0;
    if (extDragInside_) return 0.0;  // the insertion mark follows the mouse
    if (!miniMode_ && (tab_ == Tab::Channels || tab_ == Tab::Lyrics)) return 0.0;  // meters, keyboards, karaoke
    if (!miniMode_ && (showLcd_ || showVk_)) return 1.0 / 30;  // display animations, keys
    return 1.0 / 20;  // the transport (seek bar, time, bar and beat), progress bars
}

void App::setMiniMode(bool on) {
    if (on == miniMode_) return;
    int w = 0, h = 0;
    glfwGetWindowSize(window_, &w, &h);
    if (on) {
        fullWindowH_ = h;
        miniFitH_ = 0;  // fitted to the content once it has been laid out
    } else {
        glfwSetWindowSizeLimits(window_, GLFW_DONT_CARE, GLFW_DONT_CARE, GLFW_DONT_CARE, GLFW_DONT_CARE);
        glfwSetWindowSize(window_, w, std::max(fullWindowH_, 480));
    }
    miniMode_ = on;
    saveRequested_ = true;
}

// The mini player's window is as tall as its content (only the width can be changed); refitted
// when the content height changes (UI scale, another monitor).
void App::fitMiniWindow(float contentHeight) {
    if (glfwGetWindowAttrib(window_, GLFW_MAXIMIZED)) {
        glfwRestoreWindow(window_);
        miniFitH_ = 0;
    }
    const int h = int(std::ceil(contentHeight));
    if (h <= 0 || h == miniFitH_) return;
    miniFitH_ = h;
    int w = 0, cur = 0;
    glfwGetWindowSize(window_, &w, &cur);
    glfwSetWindowSizeLimits(window_, GLFW_DONT_CARE, h, GLFW_DONT_CARE, h);
    glfwSetWindowSize(window_, w, h);
}

void App::applySoundBank() {
    outputs_->setSoftSynthBank(soundBank_);
    soundBankError_ = outputs_->softSynthBankError();
    // The rebuilt synth starts empty: seeking to where the song is re-sends its programs,
    // controllers and SysEx (the same "chase" as after any seek).
    if (outputs_->usesSoftSynth() && file_) {
        outputs_->preloadSoftSynth(*file_);
        player_->seek(player_->positionUs());
    }
    saveRequested_ = true;
}

// The song as an SMF for export: a loop marked the RPG Maker way, EMIDI tracks for other devices left out.
bool App::songSmfForExport(std::vector<uint8_t>& out, std::string& error) const {
    if (!file_) {
        error = "No song loaded";
        return false;
    }
    std::vector<uint8_t> smf;
    if (!smfBytesForFile(file_->path, smf, error)) return false;
    bool loop = file_->hasLoop();
    std::vector<bool> excluded;
    for (const TrackStat& t : file_->trackStats) excluded.push_back(t.excluded);
    return applyRpgMakerLoop(smf, loop ? file_->loopStartTick : -1, loop ? file_->loopEndTick : -1, out, error, file_->emidi, excluded);
}

bool App::exportSmf(const std::string& path, std::string& error) {
    std::vector<uint8_t> out;
    if (!songSmfForExport(out, error)) return false;
    if (!writeFileAtomic(path, std::string(out.begin(), out.end()))) {
        error = "Could not write " + path;
        return false;
    }
    return true;
}

// As the target module receives the song with emulation (on, whatever the setting): the conversion of
// the song's standard (or the module the song is set to be made for) to the device in use, with the
// conversion tables and the tone map forcer as set.
bool App::exportSmfEmulated(const std::string& path, std::string& error) {
    std::vector<uint8_t> smf, out;
    if (!songSmfForExport(smf, error)) return false;
    DeviceKind target = player_->device();
    Emulator emu;
    emu.configure(info_.standard, deviceProfile(target), forcedToneMap_);
    if (auto tables = player_->conversionTables())
        emu.setConversion(tables, ConversionTables::setupFor(info_.standard, info_.suggestedDevice, target, emu.forcedToneMap()));
    if (!applyEmulation(smf, emu, out, error)) return false;
    if (!writeFileAtomic(path, std::string(out.begin(), out.end()))) {
        error = "Could not write " + path;
        return false;
    }
    return true;
}

void App::handleDialogResult(DialogPurpose purpose, const std::vector<std::string>& sel, bool recursive) {
    if (sel.empty()) return;
    std::error_code ec;
    std::filesystem::path first = pathFromUtf8(sel[0]);
    lastDir_ = pathToUtf8(std::filesystem::is_directory(first, ec) ? first : first.parent_path());
    switch (purpose) {
    case DialogPurpose::OpenFiles: openPaths(sel, true, true); break;
    case DialogPurpose::AddFiles: openPaths(sel, false, false); break;
    case DialogPurpose::AddFolder:
        for (const std::string& d : sel) playlist_.addDirectory(d, recursive);
        if (playlist_.current < 0 && !playlist_.entries.empty()) loadIndex(0, false);
        break;
    case DialogPurpose::ImportPlaylist:
        player_->stop();
        if (playlist_.load(sel[0]) && !playlist_.entries.empty()) loadIndex(0, false);
        break;
    case DialogPurpose::AppendPlaylist: {
        Playlist tmp;
        if (tmp.load(sel[0]))
            for (auto& e : tmp.entries) playlist_.add(std::move(e));
        if (playlist_.current < 0 && !playlist_.entries.empty()) loadIndex(0, false);
        break;
    }
    case DialogPurpose::ExportPlaylist: {
        std::string p = sel[0];
        if (!endsWithNoCase(p, ".m3u") && !endsWithNoCase(p, ".m3u8")) p += ".m3u";
        if (!playlist_.exportTo(p, exportRelative_)) loadError_ = "Could not write " + p;
        break;
    }
    case DialogPurpose::SoundBank:
        soundBank_ = sel[0];
        applySoundBank();
        break;
    case DialogPurpose::ExportSmf:
    case DialogPurpose::ExportSmfEmulated: {
        std::string p = sel[0], err;
        if (!endsWithNoCase(p, ".mid") && !endsWithNoCase(p, ".midi")) p += ".mid";
        bool ok = purpose == DialogPurpose::ExportSmf ? exportSmf(p, err) : exportSmfEmulated(p, err);
        if (!ok) loadError_ = err;
        break;
    }
    default: break;
    }
}

// ---------------------------------------------------------------- menu

void App::drawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    bool dialogOpen = nativeDialogs_.busy() || browser_.isOpen();
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open files...", "Ctrl+O", false, !dialogOpen)) showFileDialog(DialogPurpose::OpenFiles);
        if (ImGui::MenuItem("Add files to playlist...", nullptr, false, !dialogOpen)) showFileDialog(DialogPurpose::AddFiles);
        if (ImGui::MenuItem("Add folder to playlist...", nullptr, false, !dialogOpen)) showFileDialog(DialogPurpose::AddFolder);
        ImGui::Separator();
        if (ImGui::MenuItem("Import playlist (.m3u / .m3u8)...", nullptr, false, !dialogOpen)) showFileDialog(DialogPurpose::ImportPlaylist);
        if (ImGui::MenuItem("Append playlist (.m3u / .m3u8)...", nullptr, false, !dialogOpen)) showFileDialog(DialogPurpose::AppendPlaylist);
        if (ImGui::MenuItem("Export playlist (.m3u / .m3u8)...", nullptr, false, !dialogOpen && !playlist_.entries.empty()))
            showFileDialog(DialogPurpose::ExportPlaylist);
        ImGui::Separator();
        if (ImGui::MenuItem("Export as Standard MIDI File (.mid)...", nullptr, false, !dialogOpen && file_ != nullptr))
            showFileDialog(DialogPurpose::ExportSmf);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Saves the song (also RCP, XMIDI, MDS, RMI) as a .mid file.\n"
                              "A loop is marked the RPG Maker way: CC#111 at the loop start, the song ends at the loop end.");
        std::string emulated = std::string("Export with emulation for ") + deviceShortName(snap_->device) + " (.mid)...";
        if (ImGui::MenuItem(emulated.c_str(), nullptr, false, !dialogOpen && file_ != nullptr)) showFileDialog(DialogPurpose::ExportSmfEmulated);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Saves the song as ImMidi sends it to the %s with emulation (whether or not emulation is on):\n"
                              "converted from %s (instruments and drum kits, drum notes, effects, resets), with the tone map\n"
                              "forcer as set. Tracks, timing and texts stay; a loop is marked as in the export above.",
                              deviceShortName(snap_->device), standardName(info_.standard));
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4")) quit_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Playback")) {
        if (ImGui::MenuItem(snap_->playing ? "Pause" : "Play", "Space")) player_->togglePlayPause();
        if (ImGui::MenuItem("Stop", ".")) player_->stop();
        if (ImGui::MenuItem("Next track", "PgDn / N")) playNext(true);
        if (ImGui::MenuItem("Previous track", "PgUp / B")) playPrevious();
        ImGui::Separator();
        if (ImGui::BeginMenu("Play mode")) {
            for (int m = 0; m < int(PlayMode::Count); m++)
                if (ImGui::MenuItem(playModeName(PlayMode(m)), nullptr, int(playlist_.mode) == m)) playlist_.mode = PlayMode(m);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Honor loop points", nullptr, &loopEnabled_)) player_->setLoop(loopEnabled_, loopRepeats_);
        ImGui::Separator();
        if (ImGui::MenuItem("Send reset now")) player_->sendResetNow();
        if (ImGui::MenuItem("Panic (all notes off)", "Esc")) player_->panic();
        if (ImGui::MenuItem("Unmute all parts")) player_->clearMutes();
        if (ImGui::MenuItem("Unsolo all parts")) player_->clearSolos();
        if (ImGui::MenuItem("Clear mute and solo")) player_->clearMuteSolo();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Device")) {
        if (ImGui::MenuItem("Auto (follow the file's standard)", nullptr, deviceSetting_ < 0)) setDevice(-1);
        ImGui::Separator();
        for (int d = 0; d < deviceCount(); d++)
            if (ImGui::MenuItem(deviceProfile(DeviceKind(d)).name, nullptr, deviceSetting_ == d)) setDevice(d);
        ImGui::Separator();
        if (ImGui::MenuItem("Emulate other standards (XG on GS, ...)", nullptr, &emulation_)) player_->setEmulation(emulation_);
        if (ImGui::BeginMenu("Force Sound Canvas tone map", profile().gsMaxMap > 0)) {
            static const char* maps[] = {"Off (use the song's maps)", "SC-55 map", "SC-88 map", "SC-88Pro map", "SC-8850 map"};
            for (int m = 0; m <= 4; m++)
                if (ImGui::MenuItem(maps[m], nullptr, forcedToneMap_ == m, m <= profile().gsMaxMap)) {
                    forcedToneMap_ = m;
                    player_->setForcedToneMap(m);
                }
            ImGui::EndMenu();
        }
        bool songMenu = ImGui::BeginMenu("Song made for", file_ != nullptr);
        if (!songMenu)
            ImGui::SetItemTooltip("The module this song is made for, when its messages do not tell: a song for the\n"
                                  "SC-8850 without a GS reset is otherwise taken for a General MIDI song.\n"
                                  "The emulation converts from it and the Auto device follows it. ImMidi\n"
                                  "remembers it for the file; the playlist's menu sets it for several songs.");
        if (songMenu) {
            songModuleMenu({file_->path});
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Reset before playback")) {
            for (int r = 0; r < int(ResetMode::Count); r++)
                if (ImGui::MenuItem(resetModeName(ResetMode(r)), nullptr, resetMode_ == r)) {
                    resetMode_ = r;
                    player_->setResetMode(ResetMode(r));
                }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Mirror effects to every port", nullptr, &mirrorEffects_)) player_->setMirrorEffects(mirrorEffects_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("For SC-88 style multi-port songs played on one single-port synth per port\n"
                              "(e.g. several Sound Canvas VA): the effects and master settings the song\n"
                              "sets for all parts go to every port. See Settings > MIDI outputs.");
        ImGui::Separator();
        if (ImGui::MenuItem("MIDI outputs...")) requestedTab_ = Tab::Settings;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Mini player", "Ctrl+Shift+M", miniMode_)) setMiniMode(!miniMode_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Only the transport and the song line, in a small window.\n"
                              "The channel view is not drawn meanwhile, which also saves processor time.");
        ImGui::Separator();
        // Choosing anything that needs the full window leaves the mini player.
        for (int m = 0; m < int(ViewMode::Count); m++)
            if (ImGui::MenuItem(viewModeName(ViewMode(m)), nullptr, int(viewMode_) == m)) {
                viewMode_ = ViewMode(m);
                setMiniMode(false);
            }
        ImGui::Separator();
        if (ImGui::MenuItem("Playlist pane", nullptr, &showPlaylistPane_) && showPlaylistPane_) setMiniMode(false);
        if (ImGui::MenuItem("Channel detail panel", nullptr, &showChannelDetail_) && showChannelDetail_) setMiniMode(false);
        if (ImGui::MenuItem("Sound Canvas display", nullptr, &showLcd_) && showLcd_) setMiniMode(false);
        if (ImGui::MenuItem("Virtual keyboard", "Ctrl+K", &showVk_) && showVk_) setMiniMode(false);
        if (ImGui::MenuItem("Dark theme", nullptr, &darkTheme_)) applyTheme();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About ImMidi")) {
            showAbout_ = true;
            setMiniMode(false);
        }
        ImGui::EndMenu();
    }
    // Right-aligned status
    std::string status = std::string("Target: ") + profile().name;
    if (snap_->emulating && snap_->source != profile().family) status += std::string("  [") + standardName(snap_->source) + " emulated]";
    float w = ImGui::CalcTextSize(status.c_str()).x;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - w - 16);
    ImGui::TextDisabled("%s", status.c_str());
    ImGui::EndMenuBar();
}

// ---------------------------------------------------------------- transport

void App::drawTransport() {
    float h = ImGui::GetFrameHeight() * 1.5f;
    ImVec2 bs(h * 1.25f, h);
    if (ui::IconButton("prev", ui::Icon::Prev, bs, false, "Previous (PgUp)")) playPrevious();
    ImGui::SameLine();
    if (ui::IconButton("play", snap_->playing ? ui::Icon::Pause : ui::Icon::Play, bs, snap_->playing, "Play / Pause (Space)")) {
        if (!file_ && !playlist_.entries.empty()) loadIndex(std::max(0, playlist_.current), true);
        else player_->togglePlayPause();
    }
    ImGui::SameLine();
    if (ui::IconButton("stop", ui::Icon::Stop, bs, false, "Stop (.)")) player_->stop();
    ImGui::SameLine();
    if (ui::IconButton("next", ui::Icon::Next, bs, false, "Next (PgDn)")) playNext(true);
    ImGui::SameLine();

    // Seek bar
    double len = file_ ? file_->lengthUs / 1e6 : 0.0;
    double pos = snap_->positionUs / 1e6;
    char timeText[64];
    snprintf(timeText, sizeof timeText, "%s / %s", formatTime(seeking_ ? seekSeconds_ : pos).c_str(), formatTime(len).c_str());
    float timeW = ImGui::CalcTextSize("00:00:00 / 00:00:00").x;
    float barW = ImGui::GetContentRegionAvail().x - timeW - 260 * uiScale_;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (h - ImGui::GetFrameHeight()) * 0.5f);
    ImGui::SetNextItemWidth(std::max(100.0f, barW));
    float v = seeking_ ? seekSeconds_ : float(pos);
    ImGui::BeginDisabled(!file_);
    if (ImGui::SliderFloat("##seek", &v, 0.0f, float(std::max(len, 0.001)), "", ImGuiSliderFlags_NoRoundToFormat)) {
        seeking_ = true;
        seekSeconds_ = v;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() || (seeking_ && !ImGui::IsItemActive())) {
        player_->seek(int64_t(double(seekSeconds_) * 1e6));
        seeking_ = false;
    }
    ImGui::EndDisabled();
    {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (file_ && file_->hasLoop() && len > 0) {
            float x0 = a.x + float(file_->loopStartUs / 1e6 / len) * (b.x - a.x);
            float x1 = a.x + float(file_->loopEndUs / 1e6 / len) * (b.x - a.x);
            dl->AddRectFilled(ImVec2(x0, b.y - 3), ImVec2(x1, b.y), IM_COL32(80, 170, 255, 150));
            dl->AddLine(ImVec2(x0, a.y), ImVec2(x0, b.y), IM_COL32(80, 170, 255, 255), 2.0f);
            dl->AddLine(ImVec2(x1, a.y), ImVec2(x1, b.y), IM_COL32(80, 170, 255, 255), 2.0f);
        }
        if (ImGui::IsItemHovered() && len > 0) {
            float t = (ImGui::GetIO().MousePos.x - a.x) / (b.x - a.x);
            ImGui::SetTooltip("%s", formatTime(std::clamp(t, 0.0f, 1.0f) * len).c_str());
        }
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(timeText);
    ImGui::SameLine();
    if (file_) {
        uint64_t tick = file_->usToTick(seeking_ ? int64_t(seekSeconds_ * 1e6) : snap_->positionUs);
        int bar, beat, sub;
        file_->tickToBarBeat(tick, bar, beat, sub);
        double bpm = file_->bpmAtTick(tick) * snap_->speed;
        const TimeSignature& ts = file_->timeSigAt(tick);
        ImGui::Text("Bar %3d:%d  %6.2f BPM  %d/%d", bar, beat, bpm, ts.num, 1 << ts.denPow);
    } else {
        ImGui::TextDisabled("Bar ---  --- BPM");
    }

    // Second row: speed, transpose, volume, play mode, loop
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Speed");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160 * uiScale_);
    if (ImGui::SliderInt("##speed", &speedPercent_, 10, 400, "%d %%")) player_->setSpeed(speedPercent_ / 100.0);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Global tempo (percentage of the song tempo). Double-click the 100%% button to reset.");
    ImGui::SameLine();
    if (ImGui::SmallButton("100%")) {
        speedPercent_ = 100;
        player_->setSpeed(1.0);
    }
    ImGui::SameLine(0, 18);
    ImGui::TextUnformatted("Transpose");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * uiScale_);
    if (ImGui::InputInt("##transpose", &transpose_, 1, 12)) {
        transpose_ = std::clamp(transpose_, -24, 24);
        player_->setTranspose(transpose_);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Global key transpose in semitones (drum parts are not transposed). Keys: + / -");
    ImGui::SameLine(0, 18);
    ImGui::TextUnformatted("Volume");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120 * uiScale_);
    if (ImGui::SliderInt("##mvol", &masterVolume_, 0, 127)) player_->setMasterVolume(masterVolume_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Master volume (Universal SysEx master volume)");
    ImGui::SameLine(0, 18);
    ImGui::SetNextItemWidth(190 * uiScale_);
    int mode = int(playlist_.mode);
    if (ImGui::Combo("##mode", &mode, [](void*, int i) { return playModeName(PlayMode(i)); }, nullptr, int(PlayMode::Count)))
        playlist_.mode = PlayMode(mode);
    ImGui::SameLine(0, 18);
    if (ImGui::Checkbox("Loop points", &loopEnabled_)) player_->setLoop(loopEnabled_, loopRepeats_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Honor loop points (loopStart/loopEnd markers, EMIDI CC#116/117, RPG Maker CC#111).\n"
                          "Repeat count is set in Settings.");
    if (file_ && file_->hasLoop()) {
        ImGui::SameLine();
        if (snap_->loopRepeats < 0) ImGui::TextDisabled("(%s, loop %d / inf)", file_->loopType.c_str(), snap_->loopsDone);
        else ImGui::TextDisabled("(%s, loop %d / %d)", file_->loopType.c_str(), snap_->loopsDone, snap_->loopRepeats);
    }
    // Global mute / solo: lit while any part is muted / soloed; a click unmutes / unsolos them all.
    int muted = 0, soloed = 0;
    for (int p = 0; p < kMaxPorts; p++) {
        muted += int(std::bitset<16>(snap_->mute[p]).count());
        soloed += int(std::bitset<16>(snap_->solo[p]).count());
    }
    const ImVec2 msSize(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    ImGui::SameLine(0, 18);
    if (ui::ToggleButton("M##all", muted > 0, msSize, IM_COL32(230, 80, 70, 255)) && muted > 0) player_->clearMutes();
    if (ImGui::IsItemHovered()) {
        if (muted > 0) ImGui::SetTooltip("%d part%s muted. Click to unmute all.", muted, muted == 1 ? " is" : "s are");
        else ImGui::SetTooltip("No part is muted.");
    }
    ImGui::SameLine(0, 4);
    if (ui::ToggleButton("S##all", soloed > 0, msSize, IM_COL32(240, 200, 60, 255)) && soloed > 0) player_->clearSolos();
    if (ImGui::IsItemHovered()) {
        if (soloed > 0) ImGui::SetTooltip("%d part%s soloed. Click to end every solo.", soloed, soloed == 1 ? " is" : "s are");
        else ImGui::SetTooltip("No part is soloed.");
    }
    ImGui::SameLine(0, 18);
    if (ui::IconButton("panic", ui::Icon::Panic, ImVec2(ImGui::GetFrameHeight() * 1.3f, ImGui::GetFrameHeight()), false, "Panic: all notes off (Esc)"))
        player_->panic();
    if (snap_->starting) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "resetting...");
    }
    if (!loadJobs_.empty()) {
        const LoadJob& j = *loadJobs_.back();
        ImGui::SameLine(0, 18);
        char buf[64];
        snprintf(buf, sizeof buf, "Loading %s", fileNameOf(j.path).c_str());
        ImGui::SetNextItemWidth(220 * uiScale_);
        ImGui::ProgressBar(std::min(1.0f, j.progress.load()), ImVec2(220 * uiScale_, 0), buf);
    }
    if (snap_->skippedNotes > 0) {
        ImGui::SameLine(0, 18);
        ImGui::TextColored(snap_->lagMs > 0 ? ImVec4(1.0f, 0.55f, 0.3f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%llu notes dropped%s",
                           (unsigned long long)snap_->skippedNotes, snap_->lagMs > 0 ? " (output lagging)" : "");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The MIDI output could not keep up, so late note-ons were dropped to stay in time.\n"
                              "Settings > Performance controls this.");
    }
}

void App::drawInfoBar() {
    ImGui::Separator();
    if (!loadError_.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", loadError_.c_str());
        return;
    }
    if (!file_) {
        ImGui::TextDisabled("No file loaded. Use File > Open, drag files onto the window, or pass them on the command line.");
        ImGui::Separator();
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.86f, 0.45f, 1.0f));
    ImGui::TextUnformatted(file_->title.empty() ? file_->fileName.c_str() : file_->title.c_str());
    ImGui::PopStyleColor();
    auto sep = [] {
        ImGui::SameLine(0, 10);
        ImGui::TextDisabled("|");
        ImGui::SameLine(0, 10);
    };
    sep();
    ImGui::Text("SMF %d, %d tracks", file_->format, file_->numTracks);
    sep();
    ImGui::Text("Resolution: %s", file_->resolutionDescription().c_str());
    uint64_t tick = file_->usToTick(snap_->positionUs);
    if (const KeySignature* ks = file_->keySigAt(tick)) {
        sep();
        ImGui::Text("Key: %s", ks->name().c_str());
    }
    sep();
    ImGui::Text("File: %s", info_.summary().c_str());
    sep();
    ImGui::Text("Ports: %d", file_->numPorts);
    sep();
    drawPolyphony();
    if (file_->emidi) {
        sep();
        ImGui::Text("EMIDI (%d tracks excluded)", file_->emidiExcludedTracks);
    }
    ImGui::Separator();
}

void App::drawPolyphony() {
    // Sounding notes, as the channels' Poly column counts them: keys held plus notes a pedal holds.
    int ports = std::clamp(snap_->numPorts, 1, kMaxPorts), total = 0, perPort[kMaxPorts] = {};
    for (int p = 0; p < ports; p++) {
        for (const ChannelState& c : snap_->ports[p].ch) perPort[p] += c.activeNotes + c.sustainedNotes;
        total += perPort[p];
    }
    // Numbers in cells of a fixed width, so the line does not shift as they change.
    const float cell = ImGui::CalcTextSize("000").x;
    auto number = [&](int v) {
        char b[16];
        snprintf(b, sizeof b, "%d", v);
        float w = ImGui::CalcTextSize(b).x;
        ImGui::SameLine(0, 4);
        if (w < cell) {
            ImGui::Dummy(ImVec2(cell - w, 0));
            ImGui::SameLine(0, 0);
        }
        ImGui::TextUnformatted(b);
    };
    ImGui::BeginGroup();
    ImGui::TextUnformatted("Poly:");
    if (ports > 1) {
        for (int p = 0; p < ports; p++) {
            ImGui::SameLine(0, p ? 10 : 6);
            ImGui::TextDisabled("%c", 'A' + p);
            number(perPort[p]);
        }
        ImGui::SameLine(0, 10);
        ImGui::TextDisabled("total");
    }
    number(total);
    ImGui::EndGroup();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(ports > 1 ? "Notes sounding now on each port and in all: keys held down and notes a pedal holds.\n"
                                      "Each channel's count is in its Poly column."
                                    : "Notes sounding now: keys held down and notes a pedal holds.\n"
                                      "Each channel's count is in its Poly column.");
}

// ---------------------------------------------------------------- about

void App::drawAbout() {
    if (showAbout_) {
        ImGui::OpenPopup("About ImMidi");
        showAbout_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("About ImMidi", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
#ifndef IMMIDI_VERSION
#define IMMIDI_VERSION "1.0"
#endif
        ImGui::Text("ImMidi %s", IMMIDI_VERSION);
        ImGui::TextDisabled("A MIDI file player for GM / GM2 / GS / XG sound modules.");
        ImGui::Separator();
        ImGui::BulletText("GUI: Dear ImGui %s", IMGUI_VERSION);
        ImGui::BulletText("MIDI I/O: RtMidi");
        {
            int pf = glfwGetPlatform();
            const char* name = pf == GLFW_PLATFORM_WAYLAND ? "Wayland" : pf == GLFW_PLATFORM_X11 ? "X11" : pf == GLFW_PLATFORM_WIN32 ? "Win32" : pf == GLFW_PLATFORM_COCOA ? "Cocoa" : "other";
            ImGui::BulletText("Windowing: GLFW %s (%s)", glfwGetVersionString(), name);
            if (activeRenderer()) ImGui::BulletText("Graphics: %s", rendererName(activeRenderer()->kind()));
        }
        ImGui::BulletText("Font: Roboto (Apache License 2.0)%s", ui::g_fonts.cjkPath.empty() ? "" : " + system CJK font");
        if (!ui::g_fonts.cjkPath.empty()) ImGui::TextDisabled("CJK font: %s", ui::g_fonts.cjkPath.c_str());
        ImGui::Separator();
        ImGui::TextUnformatted("Shortcuts: Space play/pause, . stop, PgUp/PgDn previous/next,\n"
                               "Left/Right seek 5 s, +/- transpose, Esc panic, Ctrl+1..7 tabs, Ctrl+O open.");
        if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

int App::portSelector(const char* id, int port) {
    if (!file_ || file_->numPorts <= 1) return 0;
    ImGui::PushID(id);
    for (int p = 0; p < file_->numPorts; p++) {
        char label[16];
        snprintf(label, sizeof label, "Port %c", 'A' + p);
        if (p) ImGui::SameLine();
        if (ImGui::RadioButton(label, port == p)) port = p;
    }
    ImGui::PopID();
    return std::min(port, file_->numPorts - 1);
}

} // namespace immidi
