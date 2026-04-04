
void IGFD::FileManager::m_SetCurrentPath(std::vector<std::string>::iterator vPathIter) {
    m_CurrentPath = ComposeNewPath(vPathIter);
    IGFD::Utils::SetBuffer(inputPathBuffer, MAX_PATH_BUFFER_SIZE, m_CurrentPath);
    inputPathActivated = true;
}

std::string IGFD::FileManager::GetResultingPath() {
    if (dLGDirectoryMode && m_SelectedFileNames.size() == 1) {  // if directory mode with selection 1
        std::string selectedDirectory = fileNameBuffer;
        std::string path              = m_CurrentPath;
        if (!selectedDirectory.empty() && selectedDirectory != ".") {
            path += IGFD::Utils::GetPathSeparator() + selectedDirectory;
        }
        return path;
    }
    return m_CurrentPath;  // if file mode
}

std::string IGFD::FileManager::GetResultingFileName(FileDialogInternal& vFileDialogInternal, IGFD_ResultMode vFlag) {
    if (!dLGDirectoryMode) {  // if not directory mode
        const auto& filename = std::string(fileNameBuffer);
        return vFileDialogInternal.filterManager.ReplaceExtentionWithCurrentFilterIfNeeded(filename, vFlag);
    }
    return "";  // directory mode
}

std::string IGFD::FileManager::GetResultingFilePathName(FileDialogInternal& vFileDialogInternal, IGFD_ResultMode vFlag) {
    if (!dLGDirectoryMode) {  // if not directory mode
        auto result                = GetResultingPath();
        const auto& file_path_name = GetResultingFileName(vFileDialogInternal, vFlag);
        if (!file_path_name.empty()) {
            if (m_FileSystemPtr != nullptr && file_path_name.find(IGFD::Utils::GetPathSeparator()) != std::string::npos &&  // check if a path
                m_FileSystemPtr->IsFileExist(file_path_name)) {                                                             // do that only if filename is a path, not only a file name
                result = file_path_name;                                                                                    // #144, exist file, so absolute, so return it (maybe set by user in inputText)
            } else {                                                                                                        // #144, else concate path with current filename
#ifdef _IGFD_UNIX_
                if (fsRoot != result)
#endif  // _IGFD_UNIX_
                {
                    result += IGFD::Utils::GetPathSeparator();
                }
                result += file_path_name;
            }
        }

        return result;  // file mode
    }
    return "";  // directory mode
}

std::map<std::string, std::string> IGFD::FileManager::GetResultingSelection(FileDialogInternal& vFileDialogInternal, IGFD_ResultMode vFlag) {
    std::map<std::string, std::string> res;
    for (const auto& selectedFileName : m_SelectedFileNames) {
        auto result = GetResultingPath();
#ifdef _IGFD_UNIX_
        if (fsRoot != result)
#endif  // _IGFD_UNIX_
        {
            result += IGFD::Utils::GetPathSeparator();
        }
        result += vFileDialogInternal.filterManager.ReplaceExtentionWithCurrentFilterIfNeeded(selectedFileName, vFlag);
        res[selectedFileName] = result;
    }
    return res;
}

void IGFD::FileDialogInternal::NewFrame() {
    canWeContinue              = true;   // reset flag for possibily validate the dialog
    isOk                       = false;  // reset dialog result
    fileManager.devicesClicked = false;
    fileManager.pathClicked    = false;

    needToExitDialog = false;

#ifdef USE_DIALOG_EXIT_WITH_KEY
    if (ImGui::IsKeyPressed(IGFD_EXIT_KEY)) {
        // we do that here with the data's defined at the last frame
        // because escape key can quit input activation and at the end of the frame all flag will be false
        // so we will detect nothing
        if (!(fileManager.inputPathActivated || searchManager.searchInputIsActive || fileInputIsActive || fileListViewIsActive)) {
            needToExitDialog = true;  // need to quit dialog
        }
    } else
#endif
    {
        searchManager.searchInputIsActive = false;
        fileInputIsActive                 = false;
        fileListViewIsActive              = false;
    }
}

void IGFD::FileDialogInternal::EndFrame() {
    // directory change
    if (fileManager.pathClicked) {
        fileManager.OpenCurrentPath(*this);
    }

    if (fileManager.devicesClicked) {
        if (fileManager.GetDevices()) {
            fileManager.ApplyFilteringOnFileList(*this);
        }
    }

    if (fileManager.inputPathActivated) {
        auto gio = ImGui::GetIO();
        if (ImGui::IsKeyReleased(ImGuiKey_Enter)) {
            fileManager.SetCurrentPath(std::string(fileManager.inputPathBuffer));
            fileManager.OpenCurrentPath(*this);
            fileManager.inputPathActivated = false;
        }
        if (ImGui::IsKeyReleased(ImGuiKey_Escape)) {
            fileManager.inputPathActivated = false;
        }
    }

    if (ImGui::IsKeyDown(ImGuiMod_Ctrl)) {
        if (ImGui::IsKeyDown(SelectAllFilesKey)) {
            fileManager.SelectAllFileNames();
        }
    }
}

void IGFD::FileDialogInternal::ResetForNewDialog() {
}

void IGFD::FileDialogInternal::configureDialog(const std::string& vKey, const std::string& vTitle, const char* vFilters, const FileDialogConfig& vConfig) {
    m_DialogConfig = vConfig;
    ResetForNewDialog();
    dLGkey   = vKey;
    dLGtitle = vTitle;

    // treatment
    if (m_DialogConfig.sidePane == nullptr) {
        m_DialogConfig.sidePaneWidth = 0.0f;
    }

    if (m_DialogConfig.filePathName.empty()) {
        if (m_DialogConfig.path.empty()) {
            fileManager.dLGpath = fileManager.GetCurrentPath();
        } else {
            fileManager.dLGpath = m_DialogConfig.path;
        }
        fileManager.SetCurrentPath(m_DialogConfig.path);
        fileManager.dLGcountSelectionMax = (size_t)m_DialogConfig.countSelectionMax;
        fileManager.SetDefaultFileName(m_DialogConfig.fileName);
    } else {
        auto ps = fileManager.GetFileSystemInstance()->ParsePathFileName(m_DialogConfig.filePathName);
        if (ps.isOk) {
            fileManager.dLGpath = ps.path;
            fileManager.SetDefaultFileName(ps.name);
            filterManager.dLGdefaultExt = "." + ps.ext;
        } else {
            fileManager.dLGpath = fileManager.GetCurrentPath();
            fileManager.SetDefaultFileName("");
            filterManager.dLGdefaultExt.clear();
        }
    }

    filterManager.dLGdefaultExt.clear();
    filterManager.ParseFilters(vFilters);
    filterManager.SetSelectedFilterWithExt(filterManager.dLGdefaultExt);
    fileManager.SetCurrentPath(fileManager.dLGpath);
    fileManager.dLGDirectoryMode     = (vFilters == nullptr);
    fileManager.dLGcountSelectionMax = m_DialogConfig.countSelectionMax;  //-V101
    fileManager.ClearAll();
    showDialog = true;
}

const IGFD::FileDialogConfig& IGFD::FileDialogInternal::getDialogConfig() const {
    return m_DialogConfig;
}

IGFD::FileDialogConfig& IGFD::FileDialogInternal::getDialogConfigRef() {
    return m_DialogConfig;
}

IGFD::ThumbnailFeature::ThumbnailFeature() {
#ifdef USE_THUMBNAILS
    m_DisplayMode = DisplayModeEnum::FILE_LIST;
#endif
}

IGFD::ThumbnailFeature::~ThumbnailFeature() = default;

void IGFD::ThumbnailFeature::m_NewThumbnailFrame(FileDialogInternal& /*vFileDialogInternal*/) {
#ifdef USE_THUMBNAILS
    m_StartThumbnailFileDatasExtraction();
#endif
}

void IGFD::ThumbnailFeature::m_EndThumbnailFrame(FileDialogInternal& vFileDialogInternal) {
#ifdef USE_THUMBNAILS
    m_ClearThumbnails(vFileDialogInternal);
#else
    (void)vFileDialogInternal;
#endif
}

void IGFD::ThumbnailFeature::m_QuitThumbnailFrame(FileDialogInternal& vFileDialogInternal) {
#ifdef USE_THUMBNAILS
    m_StopThumbnailFileDatasExtraction();
    m_ClearThumbnails(vFileDialogInternal);
#else
    (void)vFileDialogInternal;
#endif
}

#ifdef USE_THUMBNAILS
void IGFD::ThumbnailFeature::m_StartThumbnailFileDatasExtraction() {
    const bool res = m_ThumbnailGenerationThread.use_count() && m_ThumbnailGenerationThread->joinable();
    if (!res) {
        m_IsWorking                 = true;
        m_CountFiles                = 0U;
        m_ThumbnailGenerationThread = std::shared_ptr<std::thread>(new std::thread(&IGFD::ThumbnailFeature::m_ThreadThumbnailFileDatasExtractionFunc, this), [this](std::thread* obj_ptr) {
            m_IsWorking = false;
            if (obj_ptr != nullptr) {
                m_ThumbnailFileDatasToGetCv.notify_all();
                obj_ptr->join();
            }
        });
    }
}

bool IGFD::ThumbnailFeature::m_StopThumbnailFileDatasExtraction() {
    const bool res = m_ThumbnailGenerationThread.use_count() && m_ThumbnailGenerationThread->joinable();
    if (res) {
        m_ThumbnailGenerationThread.reset();
    }
    return res;
}

void IGFD::ThumbnailFeature::m_ThreadThumbnailFileDatasExtractionFunc() {
    m_CountFiles = 0U;
    m_IsWorking  = true;
    // infinite loop while is thread working
    while (m_IsWorking) {
        std::unique_lock<std::mutex> thumbnailFileDatasToGetLock(m_ThumbnailFileDatasToGetMutex);
        m_ThumbnailFileDatasToGetCv.wait(thumbnailFileDatasToGetLock);
        if (!m_ThumbnailFileDatasToGet.empty()) {
            std::shared_ptr<FileInfos> file = nullptr;
            // get the first file in the list
            file = (*m_ThumbnailFileDatasToGet.begin());
            m_ThumbnailFileDatasToGet.pop_front();
            thumbnailFileDatasToGetLock.unlock();
            // retrieve datas of the texture file if its an image file
            if (file.use_count()) {
                if (file->fileType.isFile()) {  //-V522
                    //|| file->fileExtLevels == ".hdr" => format float so in few times
                    if (file->SearchForExts(".png,.bmp,.tga,.jpg,.jpeg,.gif,.psd,.pic,.ppm,.pgm", true)) {
                        auto fpn       = file->filePath + IGFD::Utils::GetPathSeparator() + file->fileNameExt;
                        int w          = 0;
                        int h          = 0;
                        int chans      = 0;
                        uint8_t* datas = stbi_load(fpn.c_str(), &w, &h, &chans, STBI_rgb_alpha);
                        if (datas != nullptr) {
                            if (w != 0 && h != 0) {
                                // resize with respect to glyph ratio
                                const float ratioX = (float)w / (float)h;
                                const float newX   = DisplayMode_ThumbailsList_ImageHeight * ratioX;
                                float newY         = w / ratioX;
                                if (newX < w) {
                                    newY = DisplayMode_ThumbailsList_ImageHeight;
                                }
                                const auto newWidth         = (int)newX;
                                const auto newHeight        = (int)newY;
                                const auto newBufSize       = (size_t)(newWidth * newHeight * 4U);  //-V112 //-V1028
                                auto resizedData            = new uint8_t[newBufSize];
                                const auto* resizeSucceeded = stbir_resize_uint8_linear(datas, w, h, 0, resizedData, newWidth, newHeight, 0, stbir_pixel_layout::STBIR_RGBA);  //-V112
                                if (resizeSucceeded != nullptr) {
                                    auto th              = &file->thumbnailInfo;
                                    th->textureFileDatas = resizedData;
                                    th->textureWidth     = newWidth;
                                    th->textureHeight    = newHeight;
                                    th->textureChannels  = 4;  //-V112
                                    // we set that at least, because will launch the gpu creation of the texture in the
                                    // main thread
                                    th->isReadyToUpload = true;
                                    // need gpu loading
                                    m_AddThumbnailToCreate(file);
                                } else {
                                    delete[] resizedData;
                                }
                            } else {
                                printf("image loading fail : w:%i h:%i c:%i\n", w, h, 4);  //-V112
                            }
                            stbi_image_free(datas);
                        }
                    }
                }
            }
        } else {
            thumbnailFileDatasToGetLock.unlock();
        }
    }
}

void IGFD::ThumbnailFeature::m_VariadicProgressBar(float fraction, const ImVec2& size_arg, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char TempBuffer[512];
    const int w = vsnprintf(TempBuffer, 511, fmt, args);
    va_end(args);
    if (w) {
        ImGui::ProgressBar(fraction, size_arg, TempBuffer);
    }
}

void IGFD::ThumbnailFeature::m_DrawThumbnailGenerationProgress() {
    if (m_ThumbnailGenerationThread.use_count() && m_ThumbnailGenerationThread->joinable()) {
        m_ThumbnailFileDatasToGetMutex.lock();
        if (!m_ThumbnailFileDatasToGet.empty()) {
            const auto p = (float)((double)m_CountFiles / (double)m_ThumbnailFileDatasToGet.size());                     // read => no thread concurency issues
            m_VariadicProgressBar(p, ImVec2(50, 0), "%u/%u", m_CountFiles, (uint32_t)m_ThumbnailFileDatasToGet.size());  // read => no thread concurency issues
            ImGui::SameLine();
        }
        m_ThumbnailFileDatasToGetMutex.unlock();
        m_ThumbnailFileDatasToGetCv.notify_all();
    }
}

void IGFD::ThumbnailFeature::m_AddThumbnailToLoad(const std::shared_ptr<FileInfos>& vFileInfos) {
    if (vFileInfos.use_count()) {
        if (vFileInfos->fileType.isFile()) {
            //|| file->fileExtLevels == ".hdr" => format float so in few times
            if (vFileInfos->SearchForExts(".png,.bmp,.tga,.jpg,.jpeg,.gif,.psd,.pic,.ppm,.pgm", true)) {
                // write => thread concurency issues
                m_ThumbnailFileDatasToGetMutex.lock();
                m_ThumbnailFileDatasToGet.push_back(vFileInfos);
                vFileInfos->thumbnailInfo.isLoadingOrLoaded = true;
                m_ThumbnailFileDatasToGetMutex.unlock();
            }
            m_ThumbnailFileDatasToGetCv.notify_all();
        }
    }
}

void IGFD::ThumbnailFeature::m_AddThumbnailToCreate(const std::shared_ptr<FileInfos>& vFileInfos) {
    if (vFileInfos.use_count()) {
        // write => thread concurency issues
        m_ThumbnailToCreateMutex.lock();
        m_ThumbnailToCreate.push_back(vFileInfos);
        m_ThumbnailToCreateMutex.unlock();
    }
}

void IGFD::ThumbnailFeature::m_AddThumbnailToDestroy(const IGFD_Thumbnail_Info& vIGFD_Thumbnail_Info) {
    // write => thread concurency issues
    m_ThumbnailToDestroyMutex.lock();
    m_ThumbnailToDestroy.push_back(vIGFD_Thumbnail_Info);
    m_ThumbnailToDestroyMutex.unlock();
}

void IGFD::ThumbnailFeature::m_DrawDisplayModeToolBar() {
    if (IMGUI_RADIO_BUTTON(DisplayMode_FilesList_ButtonString, m_DisplayMode == DisplayModeEnum::FILE_LIST)) m_DisplayMode = DisplayModeEnum::FILE_LIST;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(DisplayMode_FilesList_ButtonHelp);
    ImGui::SameLine();
    if (IMGUI_RADIO_BUTTON(DisplayMode_ThumbailsList_ButtonString, m_DisplayMode == DisplayModeEnum::THUMBNAILS_LIST)) m_DisplayMode = DisplayModeEnum::THUMBNAILS_LIST;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(DisplayMode_ThumbailsList_ButtonHelp);
    ImGui::SameLine();
    /* todo
    if (IMGUI_RADIO_BUTTON(DisplayMode_ThumbailsGrid_ButtonString,
        m_DisplayMode == DisplayModeEnum::THUMBNAILS_GRID))
        m_DisplayMode = DisplayModeEnum::THUMBNAILS_GRID;
    if (ImGui::IsItemHovered())	ImGui::SetTooltip(DisplayMode_ThumbailsGrid_ButtonHelp);
    ImGui::SameLine();
    */
    m_DrawThumbnailGenerationProgress();
}

void IGFD::ThumbnailFeature::m_ClearThumbnails(FileDialogInternal& vFileDialogInternal) {
    // directory wil be changed so the file list will be erased
    if (vFileDialogInternal.fileManager.pathClicked) {
        size_t count = vFileDialogInternal.fileManager.GetFullFileListSize();
        for (size_t idx = 0U; idx < count; idx++) {
            auto file = vFileDialogInternal.fileManager.GetFullFileAt(idx);
            if (file.use_count()) {
                if (file->thumbnailInfo.isReadyToDisplay)  //-V522
                {
                    m_AddThumbnailToDestroy(file->thumbnailInfo);
                }
            }
        }
    }
}

void IGFD::ThumbnailFeature::SetCreateThumbnailCallback(const CreateThumbnailFun& vCreateThumbnailFun) {
    m_CreateThumbnailFun = vCreateThumbnailFun;
}

void IGFD::ThumbnailFeature::SetDestroyThumbnailCallback(const DestroyThumbnailFun& vCreateThumbnailFun) {
    m_DestroyThumbnailFun = vCreateThumbnailFun;
}

void IGFD::ThumbnailFeature::ManageGPUThumbnails() {
    if (m_CreateThumbnailFun) {
        m_ThumbnailToCreateMutex.lock();
        if (!m_ThumbnailToCreate.empty()) {
            for (const auto& file : m_ThumbnailToCreate) {
                if (file.use_count()) {
                    m_CreateThumbnailFun(&file->thumbnailInfo);
                }
            }
            m_ThumbnailToCreate.clear();
        }
        m_ThumbnailToCreateMutex.unlock();
    } else {
        printf(
            "No Callback found for create texture\nYou need to define the callback with a call to "
            "SetCreateThumbnailCallback\n");
    }

    if (m_DestroyThumbnailFun) {
        m_ThumbnailToDestroyMutex.lock();
        if (!m_ThumbnailToDestroy.empty()) {
            for (auto thumbnail : m_ThumbnailToDestroy) {
                m_DestroyThumbnailFun(&thumbnail);
            }
            m_ThumbnailToDestroy.clear();
        }
        m_ThumbnailToDestroyMutex.unlock();
    } else {
        printf(
            "No Callback found for destroy texture\nYou need to define the callback with a call to "
            "SetCreateThumbnailCallback\n");
    }
}

#endif  // USE_THUMBNAILS

IGFD::PlacesFeature::PlacesFeature() {
#ifdef USE_PLACES_FEATURE
    m_PlacesPaneWidth = defaultPlacePaneWith;
    m_PlacesPaneShown = PLACES_PANE_DEFAULT_SHOWN;
#endif  // USE_PLACES_FEATURE
}

#ifdef USE_PLACES_FEATURE
void IGFD::PlacesFeature::m_InitPlaces(FileDialogInternal& vFileDialogInternal) {
#ifdef USE_PLACES_BOOKMARKS
    (void)vFileDialogInternal;  // for disable compiler warning about unused var
    AddPlacesGroup(placesBookmarksGroupName, placesBookmarksDisplayOrder, true, PLACES_BOOKMARK_DEFAULT_OPEPEND);
#endif  // USE_PLACES_BOOKMARK
#ifdef USE_PLACES_DEVICES
    AddPlacesGroup(placesDevicesGroupName, placesDevicesDisplayOrder, false, PLACES_DEVICES_DEFAULT_OPEPEND);
    auto devices_ptr = GetPlacesGroupPtr(placesDevicesGroupName);
    if (devices_ptr != nullptr && vFileDialogInternal.fileManager.GetFileSystemInstance() != nullptr) {
        const auto& devices = vFileDialogInternal.fileManager.GetFileSystemInstance()->GetDevicesList();
        for (const auto& device : devices) {
            devices_ptr->AddPlace(device.first + " " + device.second, device.first + IGFD::Utils::GetPathSeparator(), false);
        }
        devices_ptr = nullptr;
    }
#endif  // USE_PLACES_DEVICES
}

void IGFD::PlacesFeature::m_DrawPlacesButton() {
    IMGUI_TOGGLE_BUTTON(placesButtonString, &m_PlacesPaneShown);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(placesButtonHelpString);
}

bool IGFD::PlacesFeature::m_DrawPlacesPane(FileDialogInternal& vFileDialogInternal, const ImVec2& vSize) {
    bool res = false;
    ImGui::BeginChild("##placespane", vSize);
    for (const auto& group : m_OrderedGroups) {
        auto group_ptr = group.second.lock();
        if (group_ptr != nullptr) {
            if (ImGui::CollapsingHeader(group_ptr->name.c_str(), group_ptr->collapsingHeaderFlag)) {
                ImGui::BeginChild(group_ptr->name.c_str(), ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);
                if (group_ptr->canBeEdited) {
                    ImGui::PushID(group_ptr.get());
                    if (IMGUI_BUTTON(addPlaceButtonString "##ImGuiFileDialogAddPlace")) {
                        if (!vFileDialogInternal.fileManager.IsComposerEmpty()) {
                            group_ptr->AddPlace(vFileDialogInternal.fileManager.GetBack(), vFileDialogInternal.fileManager.GetCurrentPath(), true);
                        }
                    }
                    if (group_ptr->selectedPlaceForEdition >= 0 && group_ptr->selectedPlaceForEdition < (int)group_ptr->places.size()) {
                        ImGui::SameLine();
                        if (IMGUI_BUTTON(removePlaceButtonString "##ImGuiFileDialogRemovePlace")) {
                            group_ptr->places.erase(group_ptr->places.begin() + group_ptr->selectedPlaceForEdition);
                            if (group_ptr->selectedPlaceForEdition == (int)group_ptr->places.size()) {
                                --group_ptr->selectedPlaceForEdition;
                            }
                        }
                        if (group_ptr->selectedPlaceForEdition >= 0 && group_ptr->selectedPlaceForEdition < (int)group_ptr->places.size()) {
                            ImGui::SameLine();
                            if (IMGUI_BUTTON(validatePlaceButtonString "##ImGuiFileDialogOkPlace")) {
                                group_ptr->places[(size_t)group_ptr->selectedPlaceForEdition].name = std::string(group_ptr->editBuffer);
                                group_ptr->selectedPlaceForEdition                                 = -1;
                            }
                            ImGui::SameLine();
                            ImGui::PushItemWidth(vSize.x - ImGui::GetCursorPosX());
                            if (ImGui::InputText("##ImGuiFileDialogPlaceEdit", group_ptr->editBuffer, MAX_FILE_DIALOG_NAME_BUFFER)) {
                                group_ptr->places[(size_t)group_ptr->selectedPlaceForEdition].name = std::string(group_ptr->editBuffer);
                            }
                            ImGui::PopItemWidth();
                        }
                    }
                    ImGui::PopID();
                    ImGui::Separator();
                }
                if (!group_ptr->places.empty()) {
                    const auto& current_path = vFileDialogInternal.fileManager.GetCurrentPath();
                    group_ptr->clipper.Begin((int)group_ptr->places.size(), ImGui::GetTextLineHeightWithSpacing());
                    while (group_ptr->clipper.Step()) {
                        for (int i = group_ptr->clipper.DisplayStart; i < group_ptr->clipper.DisplayEnd; i++) {
                            if (i < 0) {
                                continue;
                            }
                            const PlaceStruct& place = group_ptr->places[(size_t)i];
                            if (place.thickness > 0.0f) {
                                ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal, place.thickness);
                            } else {
                                ImGui::PushID(i);
                                std::string place_name = place.name;
                                if (!place.style.icon.empty()) {
                                    place_name = place.style.icon + " " + place_name;
                                }
                                if (group_ptr->canBeEdited) {
                                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                                    if (ImGui::SmallButton(editPlaceButtonString "##ImGuiFileDialogPlaceEditButton")) {
                                        group_ptr->selectedPlaceForEdition = i;
                                        IGFD::Utils::ResetBuffer(group_ptr->editBuffer);
                                        IGFD::Utils::AppendToBuffer(group_ptr->editBuffer, MAX_FILE_DIALOG_NAME_BUFFER, place.name);
                                    }
                                    ImGui::PopStyleVar();
                                    ImGui::PopStyleColor();
                                    ImGui::SameLine();
                                }
                                if (ImGui::Selectable(place_name.c_str(), current_path == place.path || group_ptr->selectedPlaceForEdition == i, ImGuiSelectableFlags_AllowDoubleClick)) {  // select if path is current
                                    if (ImGui::IsMouseDoubleClicked(0)) {
                                        group_ptr->selectedPlaceForEdition = -1;  // stop edition
                                        // apply path
                                        vFileDialogInternal.fileManager.SetCurrentPath(place.path);
                                        vFileDialogInternal.fileManager.OpenCurrentPath(vFileDialogInternal);
                                        res = true;
                                    }
                                }
                                ImGui::PopID();
                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip("%s", place.path.c_str());
                                }
                            }
                        }
                    }
                    group_ptr->clipper.End();
                }
                ImGui::EndChild();
            }
        }
    }
    ImGui::EndChild();
    return res;
}

std::string IGFD::PlacesFeature::SerializePlaces(const bool /*vForceSerialisationForAll*/) {
    std::string res;
    size_t idx = 0;
    for (const auto& group : m_Groups) {
        if (group.second->canBeSaved) {
            // ## is used because reserved by imgui, so an input text cannot have ##
            res += "###" + group.first + "###";
            for (const auto& place : group.second->places) {
                if (place.canBeSaved) {
                    if (idx++ != 0) res += "##";
                    res += place.name + "##" + place.path;
                }
            }
        }
    }
    return res;
}

void IGFD::PlacesFeature::DeserializePlaces(const std::string& vPlaces) {
    if (!vPlaces.empty()) {
        const auto& groups = IGFD::Utils::SplitStringToVector(vPlaces, "###", false);
        if (groups.size() > 1) {
            for (size_t i = 0; i < groups.size(); i += 2) {
                auto group_ptr = GetPlacesGroupPtr(groups[i]);
                if (group_ptr != nullptr) {
                    const auto& places = IGFD::Utils::SplitStringToVector(groups[i + 1], "##", false);
                    if (places.size() > 1) {
                        for (size_t j = 0; j < places.size(); j += 2) {
                            group_ptr->AddPlace(places[j], places[j + 1], true);  // was saved so we set canBeSaved to true
                        }
                    }
                }
            }
        }
    }
}

bool IGFD::PlacesFeature::AddPlacesGroup(const std::string& vGroupName, const size_t& vDisplayOrder, const bool vCanBeEdited, const bool vOpenedByDefault) {
    if (vGroupName.empty()) {
        return false;
    }
    auto group_ptr           = std::make_shared<GroupStruct>();
    group_ptr->displayOrder  = vDisplayOrder;
    group_ptr->name          = vGroupName;
    group_ptr->defaultOpened = vOpenedByDefault;
    if (group_ptr->defaultOpened) {
        group_ptr->collapsingHeaderFlag = ImGuiTreeNodeFlags_DefaultOpen;
    }
    group_ptr->canBeSaved = group_ptr->canBeEdited = vCanBeEdited;  // can be user edited mean can be saved
    m_Groups[vGroupName]                           = group_ptr;
    m_OrderedGroups[group_ptr->displayOrder]       = group_ptr;  // an exisitng display order will be overwrote for code simplicity
    return true;
}

bool IGFD::PlacesFeature::RemovePlacesGroup(const std::string& vGroupName) {
    for (auto it = m_Groups.begin(); it != m_Groups.end(); ++it) {
        if ((*it).second->name == vGroupName) {
            m_Groups.erase(it);
            return true;
        }
    }
    return false;
}

IGFD::PlacesFeature::GroupStruct* IGFD::PlacesFeature::GetPlacesGroupPtr(const std::string& vGroupName) {
    if (m_Groups.find(vGroupName) != m_Groups.end()) {
        return m_Groups.at(vGroupName).get();
    }
    return nullptr;
}

bool IGFD::PlacesFeature::GroupStruct::AddPlace(const std::string& vPlaceName, const std::string& vPlacePath, const bool vCanBeSaved, const FileStyle& vStyle) {
    if (vPlaceName.empty() || vPlacePath.empty()) {
        return false;
    }
    canBeSaved |= vCanBeSaved;  // if one place must be saved so we mark the group to be saved
    PlaceStruct place;
    place.name       = vPlaceName;
    place.path       = vPlacePath;
    place.canBeSaved = vCanBeSaved;
    place.style      = vStyle;
    places.push_back(place);
    return true;
}

void IGFD::PlacesFeature::GroupStruct::AddPlaceSeparator(const float& vThickness) {
    PlaceStruct place;
    place.thickness = vThickness;
    places.push_back(place);
}

bool IGFD::PlacesFeature::GroupStruct::RemovePlace(const std::string& vPlaceName) {
    if (vPlaceName.empty()) {
        return false;
    }
    for (auto places_it = places.begin(); places_it != places.end(); ++places_it) {
        if ((*places_it).name == vPlaceName) {
            places.erase(places_it);
            return true;
        }
    }
    return false;
}
#endif  // USE_PLACES_FEATURE

IGFD::KeyExplorerFeature::KeyExplorerFeature() = default;

#ifdef USE_EXPLORATION_BY_KEYS
bool IGFD::KeyExplorerFeature::m_LocateItem_Loop(FileDialogInternal& vFileDialogInternal, ImWchar vC) {
    bool found = false;

    auto& fdi = vFileDialogInternal.fileManager;
    if (!fdi.IsFilteredListEmpty()) {
        auto countFiles = fdi.GetFilteredListSize();
        for (size_t i = m_LocateFileByInputChar_lastFileIdx; i < countFiles; i++) {
            auto nfo = fdi.GetFilteredFileAt(i);
            if (nfo.use_count()) {
                if (nfo->fileNameExt_optimized[0] == vC ||  // lower case search //-V522
                    nfo->fileNameExt[0] == vC)              // maybe upper case search
                {
                    // float p = ((float)i) * ImGui::GetTextLineHeightWithSpacing();
                    float p = (float)((double)i / (double)countFiles) * ImGui::GetScrollMaxY();
                    ImGui::SetScrollY(p);
                    m_LocateFileByInputChar_lastFound   = true;
                    m_LocateFileByInputChar_lastFileIdx = i;
                    m_StartFlashItem(m_LocateFileByInputChar_lastFileIdx);

                    auto infos_ptr = fdi.GetFilteredFileAt(m_LocateFileByInputChar_lastFileIdx);
                    if (infos_ptr.use_count()) {
                        if (infos_ptr->fileType.isDir())  //-V522
                        {
                            if (fdi.dLGDirectoryMode)  // directory chooser
                            {
                                fdi.SelectFileName(infos_ptr);
                            }
                        } else {
                            fdi.SelectFileName(infos_ptr);
                        }

                        found = true;
                        break;
                    }
                }
            }
        }
    }

    return found;
}

void IGFD::KeyExplorerFeature::m_LocateByInputKey(FileDialogInternal& vFileDialogInternal) {
    ImGuiContext& g = *GImGui;
    auto& fdi       = vFileDialogInternal.fileManager;
    if (!g.ActiveId && !fdi.IsFilteredListEmpty()) {
        auto& queueChar = ImGui::GetIO().InputQueueCharacters;
        auto countFiles = fdi.GetFilteredListSize();

        // point by char
        if (!queueChar.empty()) {
            ImWchar c = queueChar.back();
            if (m_LocateFileByInputChar_InputQueueCharactersSize != queueChar.size()) {
                if (c == m_LocateFileByInputChar_lastChar)  // next file starting with same char until
                {
                    if (m_LocateFileByInputChar_lastFileIdx < countFiles - 1U)
                        m_LocateFileByInputChar_lastFileIdx++;
                    else
                        m_LocateFileByInputChar_lastFileIdx = 0;
                }

                if (!m_LocateItem_Loop(vFileDialogInternal, c)) {
                    // not found, loop again from 0 this time
                    m_LocateFileByInputChar_lastFileIdx = 0;
                    m_LocateItem_Loop(vFileDialogInternal, c);
                }

                m_LocateFileByInputChar_lastChar = c;
            }
        }

        m_LocateFileByInputChar_InputQueueCharactersSize = queueChar.size();
    }
}

void IGFD::KeyExplorerFeature::m_ExploreWithkeys(FileDialogInternal& vFileDialogInternal, ImGuiID vListViewID) {
    auto& fdi = vFileDialogInternal.fileManager;
    if (!fdi.IsFilteredListEmpty()) {
        bool canWeExplore = false;
        bool hasNav       = (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard);

        ImGuiContext& g = *GImGui;
        if (!hasNav && !g.ActiveId)  // no nav and no activated inputs
            canWeExplore = true;

        if (g.NavId && g.NavId == vListViewID) {
            if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) || ImGui::IsKeyPressed(ImGuiKey_Space)) {
                ImGui::ActivateItemByID(vListViewID);
                ImGui::SetActiveID(vListViewID, g.CurrentWindow);
            }
        }

        if (vListViewID == g.LastActiveId - 1)  // if listview id is the last acticated nav id (ImGui::ActivateItemByID(vListViewID);)
            canWeExplore = true;

        if (canWeExplore && ImGui::IsWindowFocused()) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                ImGui::ClearActiveID();
                g.LastActiveId = 0;
            }

            auto countFiles = fdi.GetFilteredListSize();

            // explore
            bool exploreByKey     = false;
            bool enterInDirectory = false;
            bool exitDirectory    = false;

            if ((hasNav && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) || (!hasNav && ImGui::IsKeyPressed(ImGuiKey_UpArrow))) {
                exploreByKey = true;
                if (m_LocateFileByInputChar_lastFileIdx > 0)
                    m_LocateFileByInputChar_lastFileIdx--;
                else
                    m_LocateFileByInputChar_lastFileIdx = countFiles - 1U;
            } else if ((hasNav && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) || (!hasNav && ImGui::IsKeyPressed(ImGuiKey_DownArrow))) {
                exploreByKey = true;
                if (m_LocateFileByInputChar_lastFileIdx < countFiles - 1U)
                    m_LocateFileByInputChar_lastFileIdx++;
                else
                    m_LocateFileByInputChar_lastFileIdx = 0U;
            } else if (ImGui::IsKeyReleased(ImGuiKey_Enter)) {
                exploreByKey     = true;
                enterInDirectory = true;
            } else if (ImGui::IsKeyReleased(ImGuiKey_Backspace)) {
                exploreByKey  = true;
                exitDirectory = true;
            }

            if (exploreByKey) {
                // float totalHeight = m_FilteredFileList.size() * ImGui::GetTextLineHeightWithSpacing();
                float p = (float)((double)m_LocateFileByInputChar_lastFileIdx / (double)(countFiles - 1U)) * ImGui::GetScrollMaxY();  // seems not udpated in tables version outside tables
                // float p = ((float)locateFileByInputChar_lastFileIdx) * ImGui::GetTextLineHeightWithSpacing();
                ImGui::SetScrollY(p);
                m_StartFlashItem(m_LocateFileByInputChar_lastFileIdx);

                auto infos_ptr = fdi.GetFilteredFileAt(m_LocateFileByInputChar_lastFileIdx);
                if (infos_ptr.use_count()) {
                    if (infos_ptr->fileType.isDir())  //-V522
                    {
                        if (!fdi.dLGDirectoryMode || enterInDirectory) {
                            if (enterInDirectory) {
                                if (fdi.SelectDirectory(infos_ptr)) {
                                    // changement de repertoire
                                    vFileDialogInternal.fileManager.OpenCurrentPath(vFileDialogInternal);
                                    if (m_LocateFileByInputChar_lastFileIdx > countFiles - 1U) {
                                        m_LocateFileByInputChar_lastFileIdx = 0;
                                    }
                                }
                            }
                        } else  // directory chooser
                        {
                            fdi.SelectFileName(infos_ptr);
                        }
                    } else {
                        fdi.SelectFileName(infos_ptr);

                        if (enterInDirectory) {
                            vFileDialogInternal.isOk = true;
                        }
                    }

                    if (exitDirectory) {
                        auto nfo_ptr         = FileInfos::create();
                        nfo_ptr->fileNameExt = "..";

                        if (fdi.SelectDirectory(nfo_ptr)) {
                            // changement de repertoire
                            vFileDialogInternal.fileManager.OpenCurrentPath(vFileDialogInternal);
                            if (m_LocateFileByInputChar_lastFileIdx > countFiles - 1U) {
                                m_LocateFileByInputChar_lastFileIdx = 0;
                            }
                        }
#ifdef _IGFD_WIN_
                        else {
                            if (fdi.GetComposerSize() == 1U) {
                                if (fdi.GetDevices()) {
                                    fdi.ApplyFilteringOnFileList(vFileDialogInternal);
                                }
                            }
                        }
#endif  // _IGFD_WIN_
                    }
                }
            }
        }
    }
}

bool IGFD::KeyExplorerFeature::m_FlashableSelectable(const char* label, bool selected, ImGuiSelectableFlags flags, bool vFlashing, const ImVec2& size_arg) {
    using namespace ImGui;

    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g         = *GImGui;
    const ImGuiStyle& style = g.Style;

    // Submit label or explicit size to ItemSize(), whereas ItemAdd() will submit a larger/spanning rectangle.
    ImGuiID id        = window->GetID(label);
    ImVec2 label_size = CalcTextSize(label, NULL, true);
    ImVec2 size(size_arg.x != 0.0f ? size_arg.x : label_size.x, size_arg.y != 0.0f ? size_arg.y : label_size.y);
    ImVec2 pos = window->DC.CursorPos;
    pos.y += window->DC.CurrLineTextBaseOffset;
    ItemSize(size, 0.0f);

    // Fill horizontal space
    // We don't support (size < 0.0f) in Selectable() because the ItemSpacing extension would make explicitly right-aligned sizes not visibly match other widgets.
    const bool span_all_columns = (flags & ImGuiSelectableFlags_SpanAllColumns) != 0;
    const float min_x           = span_all_columns ? window->ParentWorkRect.Min.x : pos.x;
    const float max_x           = span_all_columns ? window->ParentWorkRect.Max.x : window->WorkRect.Max.x;
    if (size_arg.x == 0.0f || (flags & ImGuiSelectableFlags_SpanAvailWidth)) size.x = ImMax(label_size.x, max_x - min_x);

    // Text stays at the submission position, but bounding box may be extended on both sides
    const ImVec2 text_min = pos;
    const ImVec2 text_max(min_x + size.x, pos.y + size.y);

    // Selectables are meant to be tightly packed together with no click-gap, so we extend their box to cover spacing between selectable.
    // FIXME: Not part of layout so not included in clipper calculation, but ItemSize currenty doesn't allow offsetting CursorPos.
    ImRect bb(min_x, pos.y, text_max.x, text_max.y);
    if ((flags & ImGuiSelectableFlags_NoPadWithHalfSpacing) == 0) {
        const float spacing_x = span_all_columns ? 0.0f : style.ItemSpacing.x;
        const float spacing_y = style.ItemSpacing.y;
        const float spacing_L = IM_TRUNC(spacing_x * 0.50f);
        const float spacing_U = IM_TRUNC(spacing_y * 0.50f);
        bb.Min.x -= spacing_L;
        bb.Min.y -= spacing_U;
        bb.Max.x += (spacing_x - spacing_L);
        bb.Max.y += (spacing_y - spacing_U);
    }
    // if (g.IO.KeyCtrl) { GetForegroundDrawList()->AddRect(bb.Min, bb.Max, IM_COL32(0, 255, 0, 255)); }

    // Modify ClipRect for the ItemAdd(), faster than doing a PushColumnsBackground/PushTableBackgroundChannel for every Selectable..
    const float backup_clip_rect_min_x = window->ClipRect.Min.x;
    const float backup_clip_rect_max_x = window->ClipRect.Max.x;
    if (span_all_columns) {
        window->ClipRect.Min.x = window->ParentWorkRect.Min.x;
        window->ClipRect.Max.x = window->ParentWorkRect.Max.x;
    }

    const bool disabled_item = (flags & ImGuiSelectableFlags_Disabled) != 0;
    const bool is_visible    = ItemAdd(bb, id, NULL, disabled_item ? (ImGuiItemFlags)ImGuiItemFlags_Disabled : ImGuiItemFlags_None);

    if (span_all_columns) {
        window->ClipRect.Min.x = backup_clip_rect_min_x;
        window->ClipRect.Max.x = backup_clip_rect_max_x;
    }

    const bool is_multi_select = (g.LastItemData.ItemFlags & ImGuiItemFlags_IsMultiSelect) != 0;
    if (!is_visible)
        if (!is_multi_select || !g.BoxSelectState.UnclipMode || !g.BoxSelectState.UnclipRect.Overlaps(bb))  // Extra layer of "no logic clip" for box-select support (would be more overhead to add to ItemAdd)
            return false;

    const bool disabled_global = (g.CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    if (disabled_item && !disabled_global)  // Only testing this as an optimization
        BeginDisabled();

    // FIXME: We can standardize the behavior of those two, we could also keep the fast path of override ClipRect + full push on render only,
    // which would be advantageous since most selectable are not selected.
    if (span_all_columns) {
        if (g.CurrentTable)
            TablePushBackgroundChannel();
        else if (window->DC.CurrentColumns)
            PushColumnsBackground();
        g.LastItemData.StatusFlags |= ImGuiItemStatusFlags_HasClipRect;
        g.LastItemData.ClipRect = window->ClipRect;
    }

    // We use NoHoldingActiveID on menus so user can click and _hold_ on a menu then drag to browse child entries
    ImGuiButtonFlags button_flags = 0;
    if (flags & ImGuiSelectableFlags_NoHoldingActiveID) {
        button_flags |= ImGuiButtonFlags_NoHoldingActiveId;
    }
    if (flags & ImGuiSelectableFlags_NoSetKeyOwner) {
        button_flags |= ImGuiButtonFlags_NoSetKeyOwner;
    }
    if (flags & ImGuiSelectableFlags_SelectOnClick) {
        button_flags |= ImGuiButtonFlags_PressedOnClick;
    }
    if (flags & ImGuiSelectableFlags_SelectOnRelease) {
        button_flags |= ImGuiButtonFlags_PressedOnRelease;
    }
    if (flags & ImGuiSelectableFlags_AllowDoubleClick) {
        button_flags |= ImGuiButtonFlags_PressedOnClickRelease | ImGuiButtonFlags_PressedOnDoubleClick;
    }
    if ((flags & ImGuiSelectableFlags_AllowOverlap) || (g.LastItemData.ItemFlags & ImGuiItemFlags_AllowOverlap)) {
        button_flags |= ImGuiButtonFlags_AllowOverlap;
    }

    // Multi-selection support (header)
    const bool was_selected = selected;
    if (is_multi_select) {
        // Handle multi-select + alter button flags for it
        MultiSelectItemHeader(id, &selected, &button_flags);
    }

    bool hovered, held;
    bool pressed = ButtonBehavior(bb, id, &hovered, &held, button_flags);

    // Multi-selection support (footer)
    if (is_multi_select) {
        MultiSelectItemFooter(id, &selected, &pressed);
    } else {
        // Auto-select when moved into
        // - This will be more fully fleshed in the range-select branch
        // - This is not exposed as it won't nicely work with some user side handling of shift/control
        // - We cannot do 'if (g.NavJustMovedToId != id) { selected = false; pressed = was_selected; }' for two reasons
        //   - (1) it would require focus scope to be set, need exposing PushFocusScope() or equivalent (e.g. BeginSelection() calling PushFocusScope())
        //   - (2) usage will fail with clipped items
        //   The multi-select API aim to fix those issues, e.g. may be replaced with a BeginSelection() API.
        if ((flags & ImGuiSelectableFlags_SelectOnNav) && g.NavJustMovedToId != 0 && g.NavJustMovedToFocusScopeId == g.CurrentFocusScopeId)
            if (g.NavJustMovedToId == id) selected = pressed = true;
    }

    //////////////////////////////////////////////////////////////////
    // this function copy ImGui::Selectable just for this line....
    hovered |= vFlashing;
    //////////////////////////////////////////////////////////////////

    // Update NavId when clicking or when Hovering (this doesn't happen on most widgets), so navigation can be resumed with gamepad/keyboard
    if (pressed || (hovered && (flags & ImGuiSelectableFlags_SetNavIdOnHover))) {
        if (!g.NavHighlightItemUnderNav && g.NavWindow == window && g.NavLayer == window->DC.NavLayerCurrent) {
            SetNavID(id, window->DC.NavLayerCurrent, g.CurrentFocusScopeId, WindowRectAbsToRel(window, bb));  // (bb == NavRect)
            g.NavCursorVisible = false;
        }
    }
    if (pressed) MarkItemEdited(id);

    if (selected != was_selected) g.LastItemData.StatusFlags |= ImGuiItemStatusFlags_ToggledSelection;

    // Render
    if (is_visible) {
        if (hovered || selected) {
            // FIXME-MULTISELECT: Styling: Color for 'selected' elements? ImGuiCol_HeaderSelected
            ImU32 col;
            if (selected && !hovered)
                col = GetColorU32(ImLerp(GetStyleColorVec4(ImGuiCol_Header), GetStyleColorVec4(ImGuiCol_HeaderHovered), 0.5f));
            else
                col = GetColorU32((held && hovered) ? ImGuiCol_HeaderActive : hovered ? ImGuiCol_HeaderHovered : ImGuiCol_Header);
            RenderFrame(bb.Min, bb.Max, col, false, 0.0f);
        }
        if (g.NavId == id) {
            ImGuiNavRenderCursorFlags flags = ImGuiNavRenderCursorFlags_Compact | ImGuiNavRenderCursorFlags_NoRounding;
            if (is_multi_select) flags |= ImGuiNavRenderCursorFlags_AlwaysDraw;  // Always show the nav rectangle
            RenderNavCursor(bb, id, flags);
        }
    }

    if (span_all_columns) {
        if (g.CurrentTable)
            TablePopBackgroundChannel();
        else if (window->DC.CurrentColumns)
            PopColumnsBackground();
    }

    if (is_visible) RenderTextClipped(text_min, text_max, label, NULL, &label_size, style.SelectableTextAlign, &bb);

    // Automatically close popups
    if (pressed && (window->Flags & ImGuiWindowFlags_Popup) && !(flags & ImGuiSelectableFlags_NoAutoClosePopups) && (g.LastItemData.ItemFlags & ImGuiItemFlags_AutoClosePopups)) CloseCurrentPopup();

    if (disabled_item && !disabled_global) EndDisabled();

    // Selectable() always returns a pressed state!
    // Users of BeginMultiSelect()/EndMultiSelect() scope: you may call ImGui::IsItemToggledSelection() to retrieve
    // selection toggle, only useful if you need that state updated (e.g. for rendering purpose) before reaching EndMultiSelect().
    IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags);
    return pressed;  //-V1020
}

void IGFD::KeyExplorerFeature::m_StartFlashItem(size_t vIdx) {
    m_FlashAlpha  = 1.0f;
    m_FlashedItem = vIdx;
}

bool IGFD::KeyExplorerFeature::m_BeginFlashItem(size_t vIdx) {
    bool res = false;

    if (m_FlashedItem == vIdx && std::abs(m_FlashAlpha - 0.0f) > 0.00001f) {
        m_FlashAlpha -= m_FlashAlphaAttenInSecs * ImGui::GetIO().DeltaTime;
        if (m_FlashAlpha < 0.0f) m_FlashAlpha = 0.0f;

        ImVec4 hov = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
        hov.w      = m_FlashAlpha;
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, hov);
        res = true;
    }

    return res;
}

void IGFD::KeyExplorerFeature::m_EndFlashItem() {
    ImGui::PopStyleColor();
}

void IGFD::KeyExplorerFeature::SetFlashingAttenuationInSeconds(float vAttenValue) {
    m_FlashAlphaAttenInSecs = 1.0f / ImMax(vAttenValue, 0.01f);
}
#endif  // USE_EXPLORATION_BY_KEYS

IGFD::FileDialog::FileDialog() : PlacesFeature(), KeyExplorerFeature(), ThumbnailFeature() {
#ifdef USE_PLACES_FEATURE
    m_InitPlaces(m_FileDialogInternal);
#endif
}
IGFD::FileDialog::~FileDialog() = default;

//////////////////////////////////////////////////////////////////////////////////////////////////
///// FILE DIALOG STANDARD DIALOG ////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////

// path and fileNameExt can be specified
void IGFD::FileDialog::OpenDialog(const std::string& vKey, const std::string& vTitle, const char* vFilters, const FileDialogConfig& vConfig) {
    if (m_FileDialogInternal.showDialog)  // if already opened, quit
        return;
    m_FileDialogInternal.configureDialog(vKey, vTitle, vFilters, vConfig);
}

//////////////////////////////////////////////////////////////////////////////////////////////////
///// FILE DIALOG DISPLAY FUNCTION ///////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////

bool IGFD::FileDialog::Display(const std::string& vKey, ImGuiWindowFlags vFlags, ImVec2 vMinSize, ImVec2 vMaxSize) {
    bool res = false;

    if (m_FileDialogInternal.showDialog && m_FileDialogInternal.dLGkey == vKey) {
        if (m_FileDialogInternal.puUseCustomLocale) setlocale(m_FileDialogInternal.localeCategory, m_FileDialogInternal.localeBegin.c_str());

        auto& fdFile   = m_FileDialogInternal.fileManager;
        auto& fdFilter = m_FileDialogInternal.filterManager;

        // to be sure than only one dialog is displayed per frame
        ImGuiContext& g = *GImGui;
        if (g.FrameCount == m_FileDialogInternal.lastImGuiFrameCount) {  // one instance was displayed this frame before
            return res;                                                  // for this key +> quit
        }
        m_FileDialogInternal.lastImGuiFrameCount = g.FrameCount;  // mark this instance as used this frame

        m_CurrentDisplayedFlags = vFlags;
        std::string name        = m_FileDialogInternal.dLGtitle + "##" + m_FileDialogInternal.dLGkey;
        if (m_FileDialogInternal.name != name) {
            fdFile.ClearComposer();
            fdFile.ClearFileLists();
        }

        m_NewFrame();

#ifdef IMGUI_HAS_VIEWPORT
        if (!ImGui::GetIO().ConfigViewportsNoDecoration) {
            // https://github.com/ocornut/imgui/issues/4534
            ImGuiWindowClass window_class;
            window_class.ViewportFlagsOverrideClear = ImGuiViewportFlags_NoDecoration;
            ImGui::SetNextWindowClass(&window_class);
        }
#endif  // IMGUI_HAS_VIEWPORT

        bool beg         = false;
        ImVec2 frameSize = ImVec2(0, 0);
        if (m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_NoDialog) {  // disable our own dialog system (standard or modal)
            frameSize = vMinSize;
            beg       = true;
        } else {
            ImGui::SetNextWindowSizeConstraints(vMinSize, vMaxSize);
            if (m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_Modal &&  // disable modal because the confirm dialog for overwrite is
                !m_FileDialogInternal.okResultToConfirm) {                                    // a new modal
                ImGui::OpenPopup(name.c_str());
                beg = ImGui::BeginPopupModal(name.c_str(), (bool*)nullptr, m_CurrentDisplayedFlags | ImGuiWindowFlags_NoScrollbar);
            } else {
                beg = ImGui::Begin(name.c_str(), (bool*)nullptr, m_CurrentDisplayedFlags | ImGuiWindowFlags_NoScrollbar);
            }
        }
        if (beg) {
#ifdef IMGUI_HAS_VIEWPORT
            // if decoration is enabled we disable the resizing feature of imgui for avoid crash with SDL2 and GLFW3
            if (ImGui::GetIO().ConfigViewportsNoDecoration) {
                m_CurrentDisplayedFlags = vFlags;
            } else {
                auto win = ImGui::GetCurrentWindowRead();
                if (win->Viewport->Idx != 0)
                    m_CurrentDisplayedFlags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar;
                else
                    m_CurrentDisplayedFlags = vFlags;
            }
#endif  // IMGUI_HAS_VIEWPORT

            ImGuiID _frameId = ImGui::GetID(name.c_str());
            if (ImGui::BeginChild(_frameId, frameSize, false, m_CurrentDisplayedFlags | ImGuiWindowFlags_NoScrollbar)) {
                m_FileDialogInternal.name = name;  //-V820
                if (fdFile.dLGpath.empty()) {
                    fdFile.dLGpath = ".";  // defaut path is '.'
                }
                fdFilter.SetDefaultFilterIfNotDefined();

                // init list of files
                if (fdFile.IsFileListEmpty() && !fdFile.showDevices) {
                    if (fdFile.dLGpath != ".")                                                      // Removes extension seperator in filename if we don't check
                        IGFD::Utils::ReplaceString(fdFile.dLGDefaultFileName, fdFile.dLGpath, "");  // local path

                    if (!fdFile.dLGDefaultFileName.empty()) {
                        fdFile.SetDefaultFileName(fdFile.dLGDefaultFileName);
                        fdFilter.SetSelectedFilterWithExt(fdFilter.dLGdefaultExt);
                    } else if (fdFile.dLGDirectoryMode)  // directory mode
                        fdFile.SetDefaultFileName(".");
                    fdFile.ScanDir(m_FileDialogInternal, fdFile.dLGpath);
                }

                // draw dialog parts
                m_DrawHeader();        // place, directory, path
                m_DrawContent();       // place, files view, side pane
                res = m_DrawFooter();  // file field, filter combobox, ok/cancel buttons

                m_EndFrame();
            }
            ImGui::EndChild();

            // for display in dialog center, the confirm to overwrite dlg
            m_FileDialogInternal.dialogCenterPos = ImGui::GetCurrentWindowRead()->ContentRegionRect.GetCenter();

            // when the confirm to overwrite dialog will appear we need to
            // disable the modal mode of the main file dialog
            // see prOkResultToConfirm under
            if (m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_Modal && !m_FileDialogInternal.okResultToConfirm) {
                ImGui::EndPopup();
            }
        }

        if (m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_NoDialog) {  // disable our own dialog system (standard or modal)
        } else {
            // same things here regarding prOkResultToConfirm
            if (!(m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_Modal) || m_FileDialogInternal.okResultToConfirm) {
                ImGui::End();
            }
        }
        // confirm the result and show the confirm to overwrite dialog if needed
        res = m_Confirm_Or_OpenOverWriteFileDialog_IfNeeded(res, vFlags);

        if (m_FileDialogInternal.puUseCustomLocale) setlocale(m_FileDialogInternal.localeCategory, m_FileDialogInternal.localeEnd.c_str());
    }

    return res;
}

void IGFD::FileDialog::m_NewFrame() {
    m_FileDialogInternal.NewFrame();
    m_NewThumbnailFrame(m_FileDialogInternal);
}

void IGFD::FileDialog::m_EndFrame() {
    m_EndThumbnailFrame(m_FileDialogInternal);
    m_FileDialogInternal.EndFrame();
}
void IGFD::FileDialog::m_QuitFrame() {
    m_QuitThumbnailFrame(m_FileDialogInternal);
}

void IGFD::FileDialog::m_DrawHeader() {
#ifdef USE_PLACES_FEATURE
    if (!(m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_DisablePlaceMode)) {
        m_DrawPlacesButton();
        ImGui::SameLine();
    }

#endif  // USE_PLACES_FEATURE

    m_FileDialogInternal.fileManager.DrawDirectoryCreation(m_FileDialogInternal);

    if (
#ifdef USE_PLACES_FEATURE
        !(m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_DisablePlaceMode) ||
#endif  // USE_PLACES_FEATURE
        !(m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_DisableCreateDirectoryButton)) {
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
    }
    m_FileDialogInternal.fileManager.DrawPathComposer(m_FileDialogInternal);

#ifdef USE_THUMBNAILS
    if (!(m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_DisableThumbnailMode)) {
        m_DrawDisplayModeToolBar();
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
    }
#endif  // USE_THUMBNAILS

    m_FileDialogInternal.searchManager.DrawSearchBar(m_FileDialogInternal);
}

void IGFD::FileDialog::m_DrawContent() {
    ImVec2 size = ImGui::GetContentRegionAvail() - ImVec2(0.0f, m_FileDialogInternal.footerHeight);

#ifdef USE_PLACES_FEATURE
    if (!(m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_DisablePlaceMode)) {
        if (m_PlacesPaneShown) {
            float otherWidth = size.x - m_PlacesPaneWidth;
            ImGui::PushID("##splitterplaces");
            IGFD::Utils::ImSplitter(true, 4.0f, &m_PlacesPaneWidth, &otherWidth, 10.0f, 10.0f + m_FileDialogInternal.getDialogConfig().sidePaneWidth, size.y);
            ImGui::PopID();
            size.x -= otherWidth;
            m_DrawPlacesPane(m_FileDialogInternal, size);
            ImGui::SameLine();
        }
    }
#endif  // USE_PLACES_FEATURE

    size.x = ImGui::GetContentRegionAvail().x - m_FileDialogInternal.getDialogConfig().sidePaneWidth;

    if (m_FileDialogInternal.getDialogConfig().sidePane) {
        ImGui::PushID("##splittersidepane");
        IGFD::Utils::ImSplitter(true, 4.0f, &size.x, &m_FileDialogInternal.getDialogConfigRef().sidePaneWidth, 10.0f, 10.0f, size.y);
        ImGui::PopID();
    }

#ifdef USE_THUMBNAILS
    if (m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_DisableThumbnailMode) {
        m_DrawFileListView(size);
    } else {
        switch (m_DisplayMode) {
            case DisplayModeEnum::FILE_LIST: m_DrawFileListView(size); break;
            case DisplayModeEnum::THUMBNAILS_LIST: m_DrawThumbnailsListView(size); break;
            case DisplayModeEnum::THUMBNAILS_GRID: m_DrawThumbnailsGridView(size);
        }
    }
#else   // USE_THUMBNAILS
    m_DrawFileListView(size);
#endif  // USE_THUMBNAILS

    if (m_FileDialogInternal.getDialogConfig().sidePane) {
        m_DrawSidePane(size.y);
    }

    if (!(m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_DisableQuickPathSelection)) {
        m_DisplayPathPopup(size);
    }
}

void IGFD::FileDialog::m_DisplayPathPopup(ImVec2 vSize) {
    ImVec2 size = ImVec2(vSize.x * 0.5f, vSize.y * 0.5f);
    if (ImGui::BeginPopup("IGFD_Path_Popup")) {
        auto& fdi = m_FileDialogInternal.fileManager;

        ImGui::PushID(this);

        static ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_Hideable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoHostExtendY;
        auto listViewID              = ImGui::GetID("##FileDialog_pathTable");
        if (ImGui::BeginTableEx("##FileDialog_pathTable", listViewID, 1, flags, size, 0.0f))  //-V112
        {
            ImGui::TableSetupScrollFreeze(0, 1);  // Make header always visible
            ImGui::TableSetupColumn(tableHeaderFileNameString, ImGuiTableColumnFlags_WidthStretch | (defaultSortOrderFilename ? ImGuiTableColumnFlags_PreferSortAscending : ImGuiTableColumnFlags_PreferSortDescending), -1, 0);

            ImGui::TableHeadersRow();

            if (!fdi.IsPathFilteredListEmpty()) {
                std::string _str;
                ImFont* _font   = nullptr;
                bool _showColor = false;

                m_PathListClipper.Begin((int)fdi.GetPathFilteredListSize(), ImGui::GetTextLineHeightWithSpacing());
                while (m_PathListClipper.Step()) {
                    for (int i = m_PathListClipper.DisplayStart; i < m_PathListClipper.DisplayEnd; i++) {
                        if (i < 0) continue;

                        auto infos_ptr = fdi.GetFilteredPathAt((size_t)i);
                        if (!infos_ptr.use_count()) continue;

                        m_BeginFileColorIconStyle(infos_ptr, _showColor, _str, &_font);

                        bool selected = fdi.IsFileNameSelected(infos_ptr->fileNameExt);  // found

                        ImGui::TableNextRow();

                        if (ImGui::TableNextColumn())  // file name
                        {
                            if (ImGui::Selectable(infos_ptr->fileNameExt.c_str(), &selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_SpanAvailWidth)) {
                                fdi.SetCurrentPath(fdi.ComposeNewPath(fdi.GetCurrentPopupComposedPath()));
                                fdi.pathClicked = fdi.SelectDirectory(infos_ptr);
                                ImGui::CloseCurrentPopup();
                            }
                        }

                        m_EndFileColorIconStyle(_showColor, _font);
                    }
                }
                m_PathListClipper.End();
            }

            ImGui::EndTable();
        }

        ImGui::PopID();

        ImGui::EndPopup();
    }
}

bool IGFD::FileDialog::m_DrawOkButton() {
    auto& fdFile = m_FileDialogInternal.fileManager;
    if (m_FileDialogInternal.canWeContinue && strlen(fdFile.fileNameBuffer)) {
        if (IMGUI_BUTTON(okButtonString "##validationdialog", ImVec2(okButtonWidth, 0.0f)) || m_FileDialogInternal.isOk) {
            m_FileDialogInternal.isOk = true;
            return true;
        }

#if !invertOkAndCancelButtons
        ImGui::SameLine();
#endif
    }

    return false;
}

bool IGFD::FileDialog::m_DrawCancelButton() {
    if (IMGUI_BUTTON(cancelButtonString "##validationdialog", ImVec2(cancelButtonWidth, 0.0f)) || m_FileDialogInternal.needToExitDialog)  // dialog exit asked
    {
        m_FileDialogInternal.isOk = false;
        return true;
    }

#if invertOkAndCancelButtons
    ImGui::SameLine();
#endif

    return false;
}

bool IGFD::FileDialog::m_DrawValidationButtons() {
    bool res = false;

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - prOkCancelButtonWidth) * okCancelButtonAlignement);

    ImGui::BeginGroup();

    if (invertOkAndCancelButtons) {
        res |= m_DrawCancelButton();
        res |= m_DrawOkButton();
    } else {
        res |= m_DrawOkButton();
        res |= m_DrawCancelButton();
    }

    ImGui::EndGroup();

    prOkCancelButtonWidth = ImGui::GetItemRectSize().x;

    return res;
}

bool IGFD::FileDialog::m_DrawFooter() {
    auto& fdFile = m_FileDialogInternal.fileManager;

    float posY = ImGui::GetCursorPos().y;  // height of last bar calc
    ImGui::AlignTextToFramePadding();
    if (!fdFile.dLGDirectoryMode)
        ImGui::Text(fileNameString);
    else  // directory chooser
        ImGui::Text(dirNameString);
    ImGui::SameLine();

    // Input file fields
    float width = ImGui::GetContentRegionAvail().x;
    if (!fdFile.dLGDirectoryMode) {
        ImGuiContext& g = *GImGui;
        width -= m_FileDialogInternal.filterManager.GetFilterComboBoxWidth() + g.Style.ItemSpacing.x;
    }

    ImGui::PushItemWidth(width);
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
    if (m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_ReadOnlyFileNameField) {
        flags |= ImGuiInputTextFlags_ReadOnly;
    }
    if (ImGui::InputText("##FileName", fdFile.fileNameBuffer, MAX_FILE_DIALOG_NAME_BUFFER, flags)) {
        m_FileDialogInternal.isOk = true;
    }
    if (ImGui::GetItemID() == ImGui::GetActiveID()) m_FileDialogInternal.fileInputIsActive = true;
    ImGui::PopItemWidth();

    // combobox of filters
    m_FileDialogInternal.filterManager.DrawFilterComboBox(m_FileDialogInternal);

    bool res                          = m_DrawValidationButtons();
    m_FileDialogInternal.footerHeight = ImGui::GetCursorPosY() - posY;
    return res;
}

bool IGFD::FileDialog::m_Selectable(int vRowIdx, const char* vLabel, bool vSelected, ImGuiSelectableFlags vFlags, const ImVec2& vSizeArg) {
    bool res = false;
#ifdef USE_EXPLORATION_BY_KEYS
    bool flashed = m_BeginFlashItem((size_t)vRowIdx);
    res = m_FlashableSelectable(vLabel, vSelected, vFlags, flashed, vSizeArg);
    if (flashed) {
        m_EndFlashItem();
    }
#else   // USE_EXPLORATION_BY_KEYS
    (void)vRowIdx;  // remove a warnings for unused var
    res = ImGui::Selectable(vLabel, vSelected, vFlags, vSizeArg);
#endif  // USE_EXPLORATION_BY_KEYS
    return res;
}

void IGFD::FileDialog::m_SelectableItem(int vRowIdx, std::shared_ptr<FileInfos> vInfos, bool vSelected, const char* vFmt, ...) {
    if (!vInfos.use_count()) return;

    auto& fdi = m_FileDialogInternal.fileManager;

    static ImGuiSelectableFlags selectableFlags = ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_SpanAvailWidth;

    va_list args;
    va_start(args, vFmt);
    vsnprintf(fdi.variadicBuffer, MAX_FILE_DIALOG_NAME_BUFFER, vFmt, args);
    va_end(args);

    float h = 0.0f;
#ifdef USE_THUMBNAILS
    if (m_DisplayMode == DisplayModeEnum::THUMBNAILS_LIST && !(m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_DisableThumbnailMode)) {
        h = DisplayMode_ThumbailsList_ImageHeight;
    }
#endif  // USE_THUMBNAILS
    if (m_Selectable(vRowIdx, fdi.variadicBuffer, vSelected, selectableFlags, ImVec2(-1.0f, h))) {
        if (vInfos->fileType.isDir()) {
            // nav system, selectable cause open directory or select directory
            if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) {
                // little fix for get back the mouse behavior in nav system
                if (ImGui::IsMouseDoubleClicked(0)) {  // 0 -> left mouse button double click
                    fdi.pathClicked = fdi.SelectDirectory(vInfos);
                } else if (fdi.dLGDirectoryMode) {  // directory chooser
                    fdi.SelectOrDeselectFileName(m_FileDialogInternal, vInfos);
                } else {
                    fdi.pathClicked = fdi.SelectDirectory(vInfos);
                }
            } else {                                   // no nav system => classic behavior
                if (ImGui::IsMouseDoubleClicked(0)) {  // 0 -> left mouse button double click
                    fdi.pathClicked = fdi.SelectDirectory(vInfos);
                } else if (fdi.dLGDirectoryMode) {  // directory chooser
                    fdi.SelectOrDeselectFileName(m_FileDialogInternal, vInfos);
                }
            }
        } else {
            fdi.SelectOrDeselectFileName(m_FileDialogInternal, vInfos);
            if (ImGui::IsMouseDoubleClicked(0)) {
                m_FileDialogInternal.isOk = true;
            }
        }
    }
}

void IGFD::FileDialog::m_DisplayFileInfosTooltip(const int32_t& /*vRowIdx*/, const int32_t& vColumnIdx, std::shared_ptr<FileInfos> vFileInfos) {
    if (ImGui::IsItemHovered()) {
        if (vFileInfos != nullptr && vFileInfos->tooltipColumn == vColumnIdx) {
            if (!vFileInfos->tooltipMessage.empty()) {
                ImGui::SetTooltip("%s", vFileInfos->tooltipMessage.c_str());
            }
        }
    }
}

void IGFD::FileDialog::m_BeginFileColorIconStyle(std::shared_ptr<FileInfos> vFileInfos, bool& vOutShowColor, std::string& vOutStr, ImFont** vOutFont) {
    vOutStr.clear();
    vOutShowColor = false;

    if (vFileInfos->fileStyle.use_count())  //-V807 //-V522
    {
        vOutShowColor = true;

        *vOutFont = vFileInfos->fileStyle->font;
    }

    if (vOutShowColor && !vFileInfos->fileStyle->icon.empty())
        vOutStr = vFileInfos->fileStyle->icon;
    else if (vFileInfos->fileType.isDir())
        vOutStr = dirEntryString;
    else if (vFileInfos->fileType.isLinkToUnknown())
        vOutStr = linkEntryString;
    else if (vFileInfos->fileType.isFile())
        vOutStr = fileEntryString;

    vOutStr += " " + vFileInfos->fileNameExt;

    if (vOutShowColor) {
        ImGui::PushStyleColor(ImGuiCol_Text, vFileInfos->fileStyle->color);
    }
    if (*vOutFont) {
#if IMGUI_VERSION_NUM < 19201
        ImGui::PushFont(*vOutFont);
#else
        ImGui::PushFont(*vOutFont, 0.0f);
#endif
    }
}

void IGFD::FileDialog::m_EndFileColorIconStyle(const bool vShowColor, ImFont* vFont) {
    if (vFont) {
        ImGui::PopFont();
    }
    if (vShowColor) {
        ImGui::PopStyleColor();
    }
}

void IGFD::FileDialog::m_drawColumnText(int /*vColIdx*/, const char* vLabel, bool /*vSelected*/, bool /*vHovered*/) {
    ImGui::Text("%s", vLabel);
}

void IGFD::FileDialog::m_DrawFileListView(ImVec2 vSize) {
    auto& fdi = m_FileDialogInternal.fileManager;

    ImGui::PushID(this);

    static ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_Hideable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoHostExtendY
#ifndef USE_CUSTOM_SORTING_ICON
                                   | ImGuiTableFlags_Sortable
#endif  // USE_CUSTOM_SORTING_ICON
        ;
    auto listViewID = ImGui::GetID("##FileDialog_fileTable");
    if (ImGui::BeginTableEx("##FileDialog_fileTable", listViewID, 4, flags, vSize, 0.0f)) {
        ImGui::TableSetupScrollFreeze(0, 1);  // Make header always visible
        ImGui::TableSetupColumn(fdi.headerFileName.c_str(), ImGuiTableColumnFlags_WidthStretch | (defaultSortOrderFilename ? ImGuiTableColumnFlags_PreferSortAscending : ImGuiTableColumnFlags_PreferSortDescending), -1, 0);
        ImGui::TableSetupColumn(fdi.headerFileType.c_str(),
                                ImGuiTableColumnFlags_WidthFixed | (defaultSortOrderType ? ImGuiTableColumnFlags_PreferSortAscending : ImGuiTableColumnFlags_PreferSortDescending) |
                                    ((m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_HideColumnType) ? ImGuiTableColumnFlags_DefaultHide : 0),
                                -1, 1);
        ImGui::TableSetupColumn(fdi.headerFileSize.c_str(),
                                ImGuiTableColumnFlags_WidthFixed | (defaultSortOrderSize ? ImGuiTableColumnFlags_PreferSortAscending : ImGuiTableColumnFlags_PreferSortDescending) |
                                    ((m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_HideColumnSize) ? ImGuiTableColumnFlags_DefaultHide : 0),
                                -1, 2);
        ImGui::TableSetupColumn(fdi.headerFileDate.c_str(),
                                ImGuiTableColumnFlags_WidthFixed | (defaultSortOrderDate ? ImGuiTableColumnFlags_PreferSortAscending : ImGuiTableColumnFlags_PreferSortDescending) |
                                    ((m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_HideColumnDate) ? ImGuiTableColumnFlags_DefaultHide : 0),
                                -1, 3);

#ifndef USE_CUSTOM_SORTING_ICON
        // Sort our data if sort specs have been changed!
        if (ImGuiTableSortSpecs* sorts_specs = ImGui::TableGetSortSpecs()) {
            if (sorts_specs->SpecsDirty && !fdi.IsFileListEmpty()) {
                bool direction = sorts_specs->Specs->SortDirection == ImGuiSortDirection_Ascending;

                if (sorts_specs->Specs->ColumnUserID == 0) {
                    fdi.sortingField        = IGFD::FileManager::SortingFieldEnum::FIELD_FILENAME;
                    fdi.sortingDirection[0] = direction;
                    fdi.SortFields(m_FileDialogInternal);
                } else if (sorts_specs->Specs->ColumnUserID == 1) {
                    fdi.sortingField        = IGFD::FileManager::SortingFieldEnum::FIELD_TYPE;
                    fdi.sortingDirection[1] = direction;
                    fdi.SortFields(m_FileDialogInternal);
                } else if (sorts_specs->Specs->ColumnUserID == 2) {
                    fdi.sortingField        = IGFD::FileManager::SortingFieldEnum::FIELD_SIZE;
                    fdi.sortingDirection[2] = direction;
                    fdi.SortFields(m_FileDialogInternal);
                } else  // if (sorts_specs->Specs->ColumnUserID == 3) => alwayd true for the moment, to uncomment if we
                        // add a fourth column
                {
                    fdi.sortingField        = IGFD::FileManager::SortingFieldEnum::FIELD_DATE;
                    fdi.sortingDirection[3] = direction;
                    fdi.SortFields(m_FileDialogInternal);
                }

                sorts_specs->SpecsDirty = false;
            }
        }

        ImGui::TableHeadersRow();
#else   // USE_CUSTOM_SORTING_ICON
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int column = 0; column < 4; column++)  //-V112
        {
            ImGui::TableSetColumnIndex(column);
            const char* column_name = ImGui::TableGetColumnName(column);  // Retrieve name passed to TableSetupColumn()
            ImGui::PushID(column);
            ImGui::TableHeader(column_name);
            ImGui::PopID();
            if (ImGui::IsItemClicked()) {
                if (column == 0) {
                    if (fdi.sortingField == IGFD::FileManager::SortingFieldEnum::FIELD_FILENAME)
                        fdi.sortingDirection[0] = !fdi.sortingDirection[0];
                    else
                        fdi.sortingField = IGFD::FileManager::SortingFieldEnum::FIELD_FILENAME;

                    fdi.SortFields(m_FileDialogInternal);
                } else if (column == 1) {
                    if (fdi.sortingField == IGFD::FileManager::SortingFieldEnum::FIELD_TYPE)
                        fdi.sortingDirection[1] = !fdi.sortingDirection[1];
                    else
                        fdi.sortingField = IGFD::FileManager::SortingFieldEnum::FIELD_TYPE;

                    fdi.SortFields(m_FileDialogInternal);
                } else if (column == 2) {
                    if (fdi.sortingField == IGFD::FileManager::SortingFieldEnum::FIELD_SIZE)
                        fdi.sortingDirection[2] = !fdi.sortingDirection[2];
                    else
                        fdi.sortingField = IGFD::FileManager::SortingFieldEnum::FIELD_SIZE;

                    fdi.SortFields(m_FileDialogInternal);
                } else  // if (column == 3) => alwayd true for the moment, to uncomment if we add a fourth column
                {
                    if (fdi.sortingField == IGFD::FileManager::SortingFieldEnum::FIELD_DATE)
                        fdi.sortingDirection[3] = !fdi.sortingDirection[3];
                    else
                        fdi.sortingField = IGFD::FileManager::SortingFieldEnum::FIELD_DATE;

                    fdi.SortFields(m_FileDialogInternal);
                }
            }
        }
#endif  // USE_CUSTOM_SORTING_ICON
        if (!fdi.IsFilteredListEmpty()) {
            std::string _str;
            ImFont* _font   = nullptr;
            bool _showColor = false;

            int column_id = 0;
            bool _rowHovered = false;
            m_FileListClipper.Begin((int)fdi.GetFilteredListSize(), ImGui::GetTextLineHeightWithSpacing());
            while (m_FileListClipper.Step()) {
                for (int i = m_FileListClipper.DisplayStart; i < m_FileListClipper.DisplayEnd; i++) {
                    if (i < 0) continue;

                    auto infos_ptr = fdi.GetFilteredFileAt((size_t)i);
                    if (!infos_ptr.use_count()) continue;

                    m_BeginFileColorIconStyle(infos_ptr, _showColor, _str, &_font);

                    bool selected = fdi.IsFileNameSelected(infos_ptr->fileNameExt);  // found

                    ImGui::TableNextRow();

                    column_id = 0;
                    _rowHovered = false;
                    if (ImGui::TableNextColumn()) {  // file name
                        if (!infos_ptr->deviceInfos.empty()) {
                            _str += " " + infos_ptr->deviceInfos;
                        }
                        m_SelectableItem(i, infos_ptr, selected, _str.c_str());
                        _rowHovered = ImGui::IsItemHovered();
                        m_DisplayFileInfosTooltip(i, column_id++, infos_ptr);
                    }
                    if (ImGui::TableNextColumn()) {  // file type
                        m_drawColumnText(column_id, infos_ptr->fileExtLevels[0].c_str(), selected, _rowHovered);
                        m_DisplayFileInfosTooltip(i, column_id++, infos_ptr);
                    }
                    if (ImGui::TableNextColumn()) {  // file size
                        if (!infos_ptr->fileType.isDir()) {
                            m_drawColumnText(column_id, infos_ptr->formatedFileSize.c_str(), selected, _rowHovered);
                        } else {
                            ImGui::TextUnformatted("");
                        }
                        m_DisplayFileInfosTooltip(i, column_id++, infos_ptr);
                    }
                    if (ImGui::TableNextColumn()) {  // file date + time
                        m_drawColumnText(column_id, infos_ptr->fileModifDate.c_str(), selected, _rowHovered);
                        m_DisplayFileInfosTooltip(i, column_id++, infos_ptr);
                    }
                    m_EndFileColorIconStyle(_showColor, _font);
                }
            }
            m_FileListClipper.End();
        }

#ifdef USE_EXPLORATION_BY_KEYS
        if (!fdi.inputPathActivated) {
            m_LocateByInputKey(m_FileDialogInternal);
            m_ExploreWithkeys(m_FileDialogInternal, listViewID);
        }
#endif  // USE_EXPLORATION_BY_KEYS

        ImGuiContext& g = *GImGui;
        if (g.LastActiveId - 1 == listViewID || g.LastActiveId == listViewID) {
            m_FileDialogInternal.fileListViewIsActive = true;
        }

        ImGui::EndTable();
    }

    ImGui::PopID();
}

#ifdef USE_THUMBNAILS
void IGFD::FileDialog::m_DrawThumbnailsListView(ImVec2 vSize) {
    auto& fdi = m_FileDialogInternal.fileManager;

    ImGui::PushID(this);

    static ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_Hideable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoHostExtendY
#ifndef USE_CUSTOM_SORTING_ICON
                                   | ImGuiTableFlags_Sortable
#endif  // USE_CUSTOM_SORTING_ICON
        ;
    auto listViewID = ImGui::GetID("##FileDialog_fileTable");
    if (ImGui::BeginTableEx("##FileDialog_fileTable", listViewID, 5, flags, vSize, 0.0f)) {
        ImGui::TableSetupScrollFreeze(0, 1);  // Make header always visible
        ImGui::TableSetupColumn(fdi.headerFileName.c_str(), ImGuiTableColumnFlags_WidthStretch | (defaultSortOrderFilename ? ImGuiTableColumnFlags_PreferSortAscending : ImGuiTableColumnFlags_PreferSortDescending), -1, 0);
        ImGui::TableSetupColumn(fdi.headerFileType.c_str(),
                                ImGuiTableColumnFlags_WidthFixed | (defaultSortOrderType ? ImGuiTableColumnFlags_PreferSortAscending : ImGuiTableColumnFlags_PreferSortDescending) |
                                    ((m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_HideColumnType) ? ImGuiTableColumnFlags_DefaultHide : 0),
                                -1, 1);
        ImGui::TableSetupColumn(fdi.headerFileSize.c_str(),
                                ImGuiTableColumnFlags_WidthFixed | (defaultSortOrderSize ? ImGuiTableColumnFlags_PreferSortAscending : ImGuiTableColumnFlags_PreferSortDescending) |
                                    ((m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_HideColumnSize) ? ImGuiTableColumnFlags_DefaultHide : 0),
                                -1, 2);
        ImGui::TableSetupColumn(fdi.headerFileDate.c_str(),
                                ImGuiTableColumnFlags_WidthFixed | (defaultSortOrderDate ? ImGuiTableColumnFlags_PreferSortAscending : ImGuiTableColumnFlags_PreferSortDescending) |
                                    ((m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_HideColumnDate) ? ImGuiTableColumnFlags_DefaultHide : 0),
                                -1, 3);
        // not needed to have an option for hide the thumbnails since this is why this view is used
        ImGui::TableSetupColumn(fdi.headerFileThumbnails.c_str(), ImGuiTableColumnFlags_WidthFixed | (defaultSortOrderThumbnails ? ImGuiTableColumnFlags_PreferSortAscending : ImGuiTableColumnFlags_PreferSortDescending), -1, 4);  //-V112

#ifndef USE_CUSTOM_SORTING_ICON
        // Sort our data if sort specs have been changed!
        if (ImGuiTableSortSpecs* sorts_specs = ImGui::TableGetSortSpecs()) {
            if (sorts_specs->SpecsDirty && !fdi.IsFileListEmpty()) {
                bool direction = sorts_specs->Specs->SortDirection == ImGuiSortDirection_Ascending;

                if (sorts_specs->Specs->ColumnUserID == 0) {
                    fdi.sortingField        = IGFD::FileManager::SortingFieldEnum::FIELD_FILENAME;
                    fdi.sortingDirection[0] = direction;
                    fdi.SortFields(m_FileDialogInternal);
                } else if (sorts_specs->Specs->ColumnUserID == 1) {
                    fdi.sortingField        = IGFD::FileManager::SortingFieldEnum::FIELD_TYPE;
                    fdi.sortingDirection[1] = direction;
                    fdi.SortFields(m_FileDialogInternal);
                } else if (sorts_specs->Specs->ColumnUserID == 2) {
                    fdi.sortingField        = IGFD::FileManager::SortingFieldEnum::FIELD_SIZE;
                    fdi.sortingDirection[2] = direction;
                    fdi.SortFields(m_FileDialogInternal);
                } else if (sorts_specs->Specs->ColumnUserID == 3) {
                    fdi.sortingField        = IGFD::FileManager::SortingFieldEnum::FIELD_DATE;
                    fdi.sortingDirection[3] = direction;
                    fdi.SortFields(m_FileDialogInternal);
                } else  // if (sorts_specs->Specs->ColumnUserID == 4) = > always true for the moment, to uncomment if we
                        // add another column
                {
                    fdi.sortingField        = IGFD::FileManager::SortingFieldEnum::FIELD_THUMBNAILS;
                    fdi.sortingDirection[4] = direction;
                    fdi.SortFields(m_FileDialogInternal);
                }

                sorts_specs->SpecsDirty = false;
            }
        }

        ImGui::TableHeadersRow();
#else   // USE_CUSTOM_SORTING_ICON
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int column = 0; column < 5; column++) {
            ImGui::TableSetColumnIndex(column);
            const char* column_name = ImGui::TableGetColumnName(column);  // Retrieve name passed to TableSetupColumn()
            ImGui::PushID(column);
            ImGui::TableHeader(column_name);
            ImGui::PopID();
            if (ImGui::IsItemClicked()) {
                if (column == 0) {
                    if (fdi.sortingField == IGFD::FileManager::SortingFieldEnum::FIELD_FILENAME)
                        fdi.sortingDirection[0] = !fdi.sortingDirection[0];
                    else
                        fdi.sortingField = IGFD::FileManager::SortingFieldEnum::FIELD_FILENAME;

                    fdi.SortFields(m_FileDialogInternal);
                } else if (column == 1) {
                    if (fdi.sortingField == IGFD::FileManager::SortingFieldEnum::FIELD_TYPE)
                        fdi.sortingDirection[1] = !fdi.sortingDirection[1];
                    else
                        fdi.sortingField = IGFD::FileManager::SortingFieldEnum::FIELD_TYPE;

                    fdi.SortFields(m_FileDialogInternal);
                } else if (column == 2) {
                    if (fdi.sortingField == IGFD::FileManager::SortingFieldEnum::FIELD_SIZE)
                        fdi.sortingDirection[2] = !fdi.sortingDirection[2];
                    else
                        fdi.sortingField = IGFD::FileManager::SortingFieldEnum::FIELD_SIZE;

                    fdi.SortFields(m_FileDialogInternal);
                } else if (column == 3) {
                    if (fdi.sortingField == IGFD::FileManager::SortingFieldEnum::FIELD_DATE)
                        fdi.sortingDirection[3] = !fdi.sortingDirection[3];
                    else
                        fdi.sortingField = IGFD::FileManager::SortingFieldEnum::FIELD_DATE;

                    fdi.SortFields(m_FileDialogInternal);
                } else  // if (sorts_specs->Specs->ColumnUserID == 4) = > always true for the moment, to uncomment if we
                        // add another column
                {
                    if (fdi.sortingField == IGFD::FileManager::SortingFieldEnum::FIELD_THUMBNAILS)
                        fdi.sortingDirection[4] = !fdi.sortingDirection[4];
                    else
                        fdi.sortingField = IGFD::FileManager::SortingFieldEnum::FIELD_THUMBNAILS;

                    fdi.SortFields(m_FileDialogInternal);
                }
            }
        }
#endif  // USE_CUSTOM_SORTING_ICON
        if (!fdi.IsFilteredListEmpty()) {
            std::string _str;
            ImFont* _font   = nullptr;
            bool _showColor = false;

            ImGuiContext& g        = *GImGui;
            const float itemHeight = ImMax(g.FontSize, DisplayMode_ThumbailsList_ImageHeight) + g.Style.ItemSpacing.y;

            int column_id = 0;
            m_FileListClipper.Begin((int)fdi.GetFilteredListSize(), itemHeight);
            while (m_FileListClipper.Step()) {
                for (int i = m_FileListClipper.DisplayStart; i < m_FileListClipper.DisplayEnd; i++) {
                    if (i < 0) continue;

                    auto infos_ptr = fdi.GetFilteredFileAt((size_t)i);
                    if (!infos_ptr.use_count()) continue;

                    m_BeginFileColorIconStyle(infos_ptr, _showColor, _str, &_font);

                    bool selected = fdi.IsFileNameSelected(infos_ptr->fileNameExt);  // found

                    ImGui::TableNextRow();

                    column_id = 0;
                    if (ImGui::TableNextColumn()) {  // file name
                        if (!infos_ptr->deviceInfos.empty()) {
                            _str += " " + infos_ptr->deviceInfos;
                        }
                        m_SelectableItem(i, infos_ptr, selected, _str.c_str());
                        m_DisplayFileInfosTooltip(i, column_id++, infos_ptr);
                    }
                    if (ImGui::TableNextColumn()) {  // file type
                        ImGui::Text("%s", infos_ptr->fileExtLevels[0].c_str());
                        m_DisplayFileInfosTooltip(i, column_id++, infos_ptr);
                    }
                    if (ImGui::TableNextColumn()) {  // file size
                        if (!infos_ptr->fileType.isDir()) {
                            ImGui::Text("%s ", infos_ptr->formatedFileSize.c_str());
                        } else {
                            ImGui::TextUnformatted("");
                        }
                        m_DisplayFileInfosTooltip(i, column_id++, infos_ptr);
                    }
                    if (ImGui::TableNextColumn()) {  // file date + time
                        ImGui::Text("%s", infos_ptr->fileModifDate.c_str());
                        m_DisplayFileInfosTooltip(i, column_id++, infos_ptr);
                    }
                    if (ImGui::TableNextColumn()) {  // file thumbnails
                        auto th = &infos_ptr->thumbnailInfo;

                        if (!th->isLoadingOrLoaded) {
                            m_AddThumbnailToLoad(infos_ptr);
                        }
                        if (th->isReadyToDisplay && th->textureID) {
                            ImGui::Image((ImTextureID)th->textureID, ImVec2((float)th->textureWidth, (float)th->textureHeight));
                        }
                        m_DisplayFileInfosTooltip(i, column_id++, infos_ptr);
                    }

                    m_EndFileColorIconStyle(_showColor, _font);
                }
            }
            m_FileListClipper.End();
        }

#ifdef USE_EXPLORATION_BY_KEYS
        if (!fdi.inputPathActivated) {
            m_LocateByInputKey(m_FileDialogInternal);
            m_ExploreWithkeys(m_FileDialogInternal, listViewID);
        }
#endif  // USE_EXPLORATION_BY_KEYS

        ImGuiContext& g = *GImGui;
        if (g.LastActiveId - 1 == listViewID || g.LastActiveId == listViewID) {
            m_FileDialogInternal.fileListViewIsActive = true;
        }

        ImGui::EndTable();
    }

    ImGui::PopID();
}

void IGFD::FileDialog::m_DrawThumbnailsGridView(ImVec2 vSize) {
    if (ImGui::BeginChild("##thumbnailsGridsFiles", vSize)) {
        // todo
    }

    ImGui::EndChild();
}

#endif

void IGFD::FileDialog::m_DrawSidePane(float vHeight) {
    ImGui::SameLine();

    ImGui::BeginChild("##FileTypes", ImVec2(0, vHeight));

    m_FileDialogInternal.getDialogConfig().sidePane(m_FileDialogInternal.filterManager.GetSelectedFilter().getFirstFilter().c_str(), m_FileDialogInternal.getDialogConfigRef().userDatas, &m_FileDialogInternal.canWeContinue);
    ImGui::EndChild();
}

void IGFD::FileDialog::Close() {
    m_FileDialogInternal.dLGkey.clear();
    m_FileDialogInternal.showDialog = false;
}

bool IGFD::FileDialog::WasOpenedThisFrame(const std::string& vKey) const {
    bool res = m_FileDialogInternal.showDialog && m_FileDialogInternal.dLGkey == vKey;
    if (res) {
        res &= m_FileDialogInternal.lastImGuiFrameCount == GImGui->FrameCount;  // return true if a dialog was displayed in this frame
    }
    return res;
}

bool IGFD::FileDialog::WasOpenedThisFrame() const {
    bool res = m_FileDialogInternal.showDialog;
    if (res) {
        res &= m_FileDialogInternal.lastImGuiFrameCount == GImGui->FrameCount;  // return true if a dialog was displayed in this frame
    }
    return res;
}

bool IGFD::FileDialog::IsOpened(const std::string& vKey) const {
    return (m_FileDialogInternal.showDialog && m_FileDialogInternal.dLGkey == vKey);
}

bool IGFD::FileDialog::IsOpened() const {
    return m_FileDialogInternal.showDialog;
}

std::string IGFD::FileDialog::GetOpenedKey() const {
    if (m_FileDialogInternal.showDialog) {
        return m_FileDialogInternal.dLGkey;
    }
    return "";
}

std::string IGFD::FileDialog::GetFilePathName(IGFD_ResultMode vFlag) {
    return m_FileDialogInternal.fileManager.GetResultingFilePathName(m_FileDialogInternal, vFlag);
}

std::string IGFD::FileDialog::GetCurrentPath() {
    return m_FileDialogInternal.fileManager.GetResultingPath();
}

std::string IGFD::FileDialog::GetCurrentFileName(IGFD_ResultMode vFlag) {
    return m_FileDialogInternal.fileManager.GetResultingFileName(m_FileDialogInternal, vFlag);
}

std::string IGFD::FileDialog::GetCurrentFilter() {
    return m_FileDialogInternal.filterManager.GetSelectedFilter().title;
}

std::map<std::string, std::string> IGFD::FileDialog::GetSelection(IGFD_ResultMode vFlag) {
    return m_FileDialogInternal.fileManager.GetResultingSelection(m_FileDialogInternal, vFlag);
}

IGFD::UserDatas IGFD::FileDialog::GetUserDatas() const {
    return m_FileDialogInternal.getDialogConfig().userDatas;
}

bool IGFD::FileDialog::IsOk() const {
    return m_FileDialogInternal.isOk;
}

void IGFD::FileDialog::SetFileStyle(const IGFD_FileStyleFlags& vFlags, const char* vCriteria, const FileStyle& vInfos) {
    m_FileDialogInternal.filterManager.SetFileStyle(vFlags, vCriteria, vInfos);
}

void IGFD::FileDialog::SetFileStyle(const IGFD_FileStyleFlags& vFlags, const char* vCriteria, const ImVec4& vColor, const std::string& vIcon, ImFont* vFont) {
    m_FileDialogInternal.filterManager.SetFileStyle(vFlags, vCriteria, vColor, vIcon, vFont);
}

void IGFD::FileDialog::SetFileStyle(FileStyle::FileStyleFunctor vFunctor) {
    m_FileDialogInternal.filterManager.SetFileStyle(vFunctor);
}

bool IGFD::FileDialog::GetFileStyle(const IGFD_FileStyleFlags& vFlags, const std::string& vCriteria, ImVec4* vOutColor, std::string* vOutIcon, ImFont** vOutFont) {
    return m_FileDialogInternal.filterManager.GetFileStyle(vFlags, vCriteria, vOutColor, vOutIcon, vOutFont);
}

void IGFD::FileDialog::ClearFilesStyle() {
    m_FileDialogInternal.filterManager.ClearFilesStyle();
}

void IGFD::FileDialog::SetLocales(const int& /*vLocaleCategory*/, const std::string& vLocaleBegin, const std::string& vLocaleEnd) {
    m_FileDialogInternal.puUseCustomLocale = true;
    m_FileDialogInternal.localeBegin       = vLocaleBegin;
    m_FileDialogInternal.localeEnd         = vLocaleEnd;
}

//////////////////////////////////////////////////////////////////////////////
//// OVERWRITE DIALOG ////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

bool IGFD::FileDialog::m_Confirm_Or_OpenOverWriteFileDialog_IfNeeded(bool vLastAction, ImGuiWindowFlags vFlags) {
    // if confirmation => return true for confirm the overwrite et quit the dialog
    // if cancel => return false && set IsOk to false for keep inside the dialog

    // if IsOk == false => return false for quit the dialog
    if (!m_FileDialogInternal.isOk && vLastAction) {
        m_QuitFrame();
        return true;
    }

    // if IsOk == true && no check of overwrite => return true for confirm the dialog
    if (m_FileDialogInternal.isOk && vLastAction && !(m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_ConfirmOverwrite)) {
        m_QuitFrame();
        return true;
    }

    // if IsOk == true && check of overwrite => return false and show confirm to overwrite dialog
    if ((m_FileDialogInternal.okResultToConfirm || (m_FileDialogInternal.isOk && vLastAction)) && (m_FileDialogInternal.getDialogConfig().flags & ImGuiFileDialogFlags_ConfirmOverwrite)) {
        if (m_FileDialogInternal.isOk)  // catched only one time
        {
            if (!m_FileDialogInternal.fileManager.GetFileSystemInstance()->IsFileExist(GetFilePathName()))  // not existing => quit dialog
            {
                m_QuitFrame();
                return true;
            } else  // existing => confirm dialog to open
            {
                m_FileDialogInternal.isOk              = false;
                m_FileDialogInternal.okResultToConfirm = true;
            }
        }

        std::string name = OverWriteDialogTitleString "##" + m_FileDialogInternal.dLGtitle + m_FileDialogInternal.dLGkey + "OverWriteDialog";

        bool res = false;

        ImGui::OpenPopup(name.c_str());
        if (ImGui::BeginPopupModal(name.c_str(), (bool*)0, vFlags | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::SetWindowPos(m_FileDialogInternal.dialogCenterPos - ImGui::GetWindowSize() * 0.5f);  // next frame needed for GetWindowSize to work

            ImGui::Text("%s", OverWriteDialogMessageString);

            if (IMGUI_BUTTON(OverWriteDialogConfirmButtonString)) {
                m_FileDialogInternal.okResultToConfirm = false;
                m_FileDialogInternal.isOk              = true;
                res                                    = true;
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();

            if (IMGUI_BUTTON(OverWriteDialogCancelButtonString)) {
                m_FileDialogInternal.okResultToConfirm = false;
                m_FileDialogInternal.isOk              = false;
                res                                    = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        if (res) {
            m_QuitFrame();
        }
        return res;
    }

    return false;
}

#endif  // __cplusplus

// return an initialized IGFD_FileDialog_Config
IGFD_C_API IGFD_FileDialog_Config IGFD_FileDialog_Config_Get() {
    IGFD_FileDialog_Config res = {};
    res.path                   = "";
    res.fileName               = "";
    res.filePathName           = "";
    res.countSelectionMax      = 1;
    res.userDatas              = nullptr;
    res.sidePane               = nullptr;
    res.sidePaneWidth          = 250.0f;
    res.flags                  = ImGuiFileDialogFlags_Default;
    return res;
}

// Return an initialized IGFD_Selection_Pair
IGFD_C_API IGFD_Selection_Pair IGFD_Selection_Pair_Get(void) {
    IGFD_Selection_Pair res = {};
    res.fileName            = nullptr;
    res.filePathName        = nullptr;
    return res;
}

// destroy only the content of vSelection_Pair
IGFD_C_API void IGFD_Selection_Pair_DestroyContent(IGFD_Selection_Pair* vSelection_Pair) {
    if (vSelection_Pair) {
        delete[] vSelection_Pair->fileName;
        delete[] vSelection_Pair->filePathName;
    }
}

// Return an initialized IGFD_Selection
IGFD_C_API IGFD_Selection IGFD_Selection_Get(void) {
    return {nullptr, 0U};
}

// destroy only the content of vSelection
IGFD_C_API void IGFD_Selection_DestroyContent(IGFD_Selection* vSelection) {
    if (vSelection) {
        if (vSelection->table) {
            for (size_t i = 0U; i < vSelection->count; i++) {
                IGFD_Selection_Pair_DestroyContent(&vSelection->table[i]);
            }
            delete[] vSelection->table;
        }
        vSelection->count = 0U;
    }
}

// create an instance of ImGuiFileDialog
IGFD_C_API ImGuiFileDialog* IGFD_Create(void) {
    return new ImGuiFileDialog();
}

// destroy the instance of ImGuiFileDialog
IGFD_C_API void IGFD_Destroy(ImGuiFileDialog* vContextPtr) {
    if (vContextPtr != nullptr) {
        delete vContextPtr;
        vContextPtr = nullptr;
    }
}

IGFD_C_API void IGFD_OpenDialog(             // open a standard dialog
    ImGuiFileDialog* vContextPtr,            // ImGuiFileDialog context
    const char* vKey,                        // key dialog
    const char* vTitle,                      // title
    const char* vFilters,                    // filters/filter collections. set it to null for directory mode
    const IGFD_FileDialog_Config vConfig) {  // path
    if (vContextPtr != nullptr) {
        IGFD::FileDialogConfig config;
        config.path              = vConfig.path;
        config.fileName          = vConfig.fileName;
        config.filePathName      = vConfig.filePathName;
        config.countSelectionMax = vConfig.countSelectionMax;
        config.userDatas         = vConfig.userDatas;
        config.flags             = vConfig.flags;
        config.sidePane          = vConfig.sidePane;
        config.sidePaneWidth     = vConfig.sidePaneWidth;
        vContextPtr->OpenDialog(vKey, vTitle, vFilters, config);
    }
}

IGFD_C_API bool IGFD_DisplayDialog(ImGuiFileDialog* vContextPtr, const char* vKey, ImGuiWindowFlags vFlags, ImVec2 vMinSize, ImVec2 vMaxSize) {
    if (vContextPtr != nullptr) {
        return vContextPtr->Display(vKey, vFlags, vMinSize, vMaxSize);
    }
    return false;
}

IGFD_C_API void IGFD_CloseDialog(ImGuiFileDialog* vContextPtr) {
    if (vContextPtr != nullptr) {
        vContextPtr->Close();
    }
}

IGFD_C_API bool IGFD_IsOk(ImGuiFileDialog* vContextPtr) {
    if (vContextPtr != nullptr) {
        return vContextPtr->IsOk();
    }
    return false;
}

IGFD_C_API bool IGFD_WasKeyOpenedThisFrame(ImGuiFileDialog* vContextPtr, const char* vKey) {
    if (vContextPtr != nullptr) {
        return vContextPtr->WasOpenedThisFrame(vKey);
    }
    return false;
}

IGFD_C_API bool IGFD_WasOpenedThisFrame(ImGuiFileDialog* vContextPtr) {
    if (vContextPtr != nullptr) {
        return vContextPtr->WasOpenedThisFrame();
    }

    return false;
}

IGFD_C_API bool IGFD_IsKeyOpened(ImGuiFileDialog* vContextPtr, const char* vCurrentOpenedKey) {
    if (vContextPtr != nullptr) {
        return vContextPtr->IsOpened(vCurrentOpenedKey);
    }

    return false;
}

IGFD_C_API bool IGFD_IsOpened(ImGuiFileDialog* vContextPtr) {
    if (vContextPtr != nullptr) {
        return vContextPtr->IsOpened();
    }

    return false;
}

IGFD_C_API IGFD_Selection IGFD_GetSelection(ImGuiFileDialog* vContextPtr, IGFD_ResultMode vMode) {
    IGFD_Selection res = IGFD_Selection_Get();
    if (vContextPtr != nullptr) {
        auto sel = vContextPtr->GetSelection(vMode);
        if (!sel.empty()) {
            res.count = sel.size();
            res.table = new IGFD_Selection_Pair[res.count];

            size_t idx = 0U;
            for (const auto& s : sel) {
                IGFD_Selection_Pair* pair = res.table + idx++;

                // fileNameExt
                if (!s.first.empty()) {
                    size_t siz     = s.first.size() + 1U;
                    pair->fileName = new char[siz];
#ifndef _MSC_VER
                    strncpy(pair->fileName, s.first.c_str(), siz);
#else   // _MSC_VER
                    strncpy_s(pair->fileName, siz, s.first.c_str(), siz);
#endif  // _MSC_VER
                    pair->fileName[siz - 1U] = '\0';
                }

                // filePathName
                if (!s.second.empty()) {
                    size_t siz         = s.second.size() + 1U;
                    pair->filePathName = new char[siz];
#ifndef _MSC_VER
                    strncpy(pair->filePathName, s.second.c_str(), siz);
#else   // _MSC_VER
                    strncpy_s(pair->filePathName, siz, s.second.c_str(), siz);
#endif  // _MSC_VER
                    pair->filePathName[siz - 1U] = '\0';
                }
            }

            return res;
        }
    }

    return res;
}

IGFD_C_API char* IGFD_GetFilePathName(ImGuiFileDialog* vContextPtr, IGFD_ResultMode vMode) {
    char* res = nullptr;

    if (vContextPtr != nullptr) {
        auto s = vContextPtr->GetFilePathName(vMode);
        if (!s.empty()) {
            size_t siz = s.size() + 1U;
            res        = (char*)malloc(siz);
            if (res) {
#ifndef _MSC_VER
                strncpy(res, s.c_str(), siz);
#else   // _MSC_VER
                strncpy_s(res, siz, s.c_str(), siz);
#endif  // _MSC_VER
                res[siz - 1U] = '\0';
            }
        }
    }

    return res;
}

IGFD_C_API char* IGFD_GetCurrentFileName(ImGuiFileDialog* vContextPtr, IGFD_ResultMode vMode) {
    char* res = nullptr;

    if (vContextPtr != nullptr) {
        auto s = vContextPtr->GetCurrentFileName(vMode);
        if (!s.empty()) {
            size_t siz = s.size() + 1U;
            res        = (char*)malloc(siz);
            if (res) {
#ifndef _MSC_VER
                strncpy(res, s.c_str(), siz);
#else   // _MSC_VER
                strncpy_s(res, siz, s.c_str(), siz);
#endif  // _MSC_VER
                res[siz - 1U] = '\0';
            }
        }
    }

    return res;
}

IGFD_C_API char* IGFD_GetCurrentPath(ImGuiFileDialog* vContextPtr) {
    char* res = nullptr;

    if (vContextPtr != nullptr) {
        auto s = vContextPtr->GetCurrentPath();
        if (!s.empty()) {
            size_t siz = s.size() + 1U;
            res        = (char*)malloc(siz);
            if (res) {
#ifndef _MSC_VER
                strncpy(res, s.c_str(), siz);
#else   // _MSC_VER
                strncpy_s(res, siz, s.c_str(), siz);
#endif  // _MSC_VER
                res[siz - 1U] = '\0';
            }
        }
    }

    return res;
}

IGFD_C_API char* IGFD_GetCurrentFilter(ImGuiFileDialog* vContextPtr) {
    char* res = nullptr;

    if (vContextPtr != nullptr) {
        auto s = vContextPtr->GetCurrentFilter();
        if (!s.empty()) {
            size_t siz = s.size() + 1U;
            res        = (char*)malloc(siz);
            if (res) {
#ifndef _MSC_VER
                strncpy(res, s.c_str(), siz);
#else   // _MSC_VER
                strncpy_s(res, siz, s.c_str(), siz);
#endif  // _MSC_VER
                res[siz - 1U] = '\0';
            }
        }
    }

    return res;
}

IGFD_C_API void* IGFD_GetUserDatas(ImGuiFileDialog* vContextPtr) {
    if (vContextPtr != nullptr) {
        return vContextPtr->GetUserDatas();
    }

    return nullptr;
}

IGFD_C_API void IGFD_SetFileStyle(ImGuiFileDialog* vContextPtr, IGFD_FileStyleFlags vFlags, const char* vCriteria, ImVec4 vColor, const char* vIcon,
                                  ImFont* vFont)  //-V813
{
    if (vContextPtr != nullptr) {
        vContextPtr->SetFileStyle(vFlags, vCriteria, vColor, vIcon, vFont);
    }
}

IGFD_C_API void IGFD_SetFileStyle2(ImGuiFileDialog* vContextPtr, IGFD_FileStyleFlags vFlags, const char* vCriteria, float vR, float vG, float vB, float vA, const char* vIcon, ImFont* vFont) {
    if (vContextPtr != nullptr) {
        vContextPtr->SetFileStyle(vFlags, vCriteria, ImVec4(vR, vG, vB, vA), vIcon, vFont);
    }
}

IGFD_C_API bool IGFD_GetFileStyle(ImGuiFileDialog* vContextPtr, IGFD_FileStyleFlags vFlags, const char* vCriteria, ImVec4* vOutColor, char** vOutIconText, ImFont** vOutFont) {
    if (vContextPtr != nullptr) {
        std::string icon;
        bool res = vContextPtr->GetFileStyle(vFlags, vCriteria, vOutColor, &icon, vOutFont);
        if (!icon.empty() && vOutIconText) {
            size_t siz    = icon.size() + 1U;
            *vOutIconText = (char*)malloc(siz);
            if (*vOutIconText) {
#ifndef _MSC_VER
                strncpy(*vOutIconText, icon.c_str(), siz);
#else   // _MSC_VER
                strncpy_s(*vOutIconText, siz, icon.c_str(), siz);
#endif  // _MSC_VER
                (*vOutIconText)[siz - 1U] = '\0';
            }
        }
        return res;
    }

    return false;
}

IGFD_C_API void IGFD_ClearFilesStyle(ImGuiFileDialog* vContextPtr) {
    if (vContextPtr != nullptr) {
        vContextPtr->ClearFilesStyle();
    }
}

IGFD_C_API void SetLocales(ImGuiFileDialog* vContextPtr, const int vCategory, const char* vBeginLocale, const char* vEndLocale) {
    if (vContextPtr != nullptr) {
        vContextPtr->SetLocales(vCategory, (vBeginLocale ? vBeginLocale : ""), (vEndLocale ? vEndLocale : ""));
    }
}

#ifdef USE_EXPLORATION_BY_KEYS
IGFD_C_API void IGFD_SetFlashingAttenuationInSeconds(ImGuiFileDialog* vContextPtr, float vAttenValue) {
    if (vContextPtr != nullptr) {
        vContextPtr->SetFlashingAttenuationInSeconds(vAttenValue);
    }
}
#endif

#ifdef USE_PLACES_FEATURE
IGFD_C_API char* IGFD_SerializePlaces(ImGuiFileDialog* vContextPtr, bool vDontSerializeCodeBasedPlaces) {
    char* res = nullptr;

    if (vContextPtr != nullptr) {
        auto s = vContextPtr->SerializePlaces(vDontSerializeCodeBasedPlaces);
        if (!s.empty()) {
            size_t siz = s.size() + 1U;
            res        = (char*)malloc(siz);
            if (res) {
#ifndef _MSC_VER
                strncpy(res, s.c_str(), siz);
#else   // _MSC_VER
                strncpy_s(res, siz, s.c_str(), siz);
#endif  // _MSC_VER
                res[siz - 1U] = '\0';
            }
        }
    }

    return res;
}

IGFD_C_API void IGFD_DeserializePlaces(ImGuiFileDialog* vContextPtr, const char* vPlaces) {
    if (vContextPtr != nullptr) {
        vContextPtr->DeserializePlaces(vPlaces);
    }
}

IGFD_C_API bool IGFD_AddPlacesGroup(ImGuiFileDialog* vContextPtr, const char* vGroupName, size_t vDisplayOrder, bool vCanBeEdited) {
    if (vContextPtr != nullptr) {
        return vContextPtr->AddPlacesGroup(vGroupName, vDisplayOrder, vCanBeEdited);
    }
    return false;
}

IGFD_C_API bool IGFD_RemovePlacesGroup(ImGuiFileDialog* vContextPtr, const char* vGroupName) {
    if (vContextPtr != nullptr) {
        return vContextPtr->RemovePlacesGroup(vGroupName);
    }
    return false;
}

IGFD_C_API bool IGFD_AddPlace(ImGuiFileDialog* vContextPtr, const char* vGroupName, const char* vPlaceName, const char* vPlacePath, bool vCanBeSaved, const char* vIconText) {
    if (vContextPtr != nullptr) {
        auto group_ptr = vContextPtr->GetPlacesGroupPtr(vGroupName);
        if (group_ptr != nullptr) {
            IGFD::FileStyle style;
            style.icon = vIconText;
            return group_ptr->AddPlace(vPlaceName, vPlacePath, vCanBeSaved, style);
        }
    }
    return false;
}

IGFD_C_API bool IGFD_RemovePlace(ImGuiFileDialog* vContextPtr, const char* vGroupName, const char* vPlaceName) {
    if (vContextPtr != nullptr) {
        auto group_ptr = vContextPtr->GetPlacesGroupPtr(vGroupName);
        if (group_ptr != nullptr) {
            return group_ptr->RemovePlace(vPlaceName);
        }
    }
    return false;
}

#endif

#ifdef USE_THUMBNAILS
IGFD_C_API void SetCreateThumbnailCallback(ImGuiFileDialog* vContextPtr, const IGFD_CreateThumbnailFun vCreateThumbnailFun) {
    if (vContextPtr != nullptr) {
        vContextPtr->SetCreateThumbnailCallback(vCreateThumbnailFun);
    }
}

IGFD_C_API void SetDestroyThumbnailCallback(ImGuiFileDialog* vContextPtr, const IGFD_DestroyThumbnailFun vDestroyThumbnailFun) {
    if (vContextPtr != nullptr) {
        vContextPtr->SetDestroyThumbnailCallback(vDestroyThumbnailFun);
    }
}

IGFD_C_API void ManageGPUThumbnails(ImGuiFileDialog* vContextPtr) {
    if (vContextPtr != nullptr) {
        vContextPtr->ManageGPUThumbnails();
    }
}
#endif  // USE_THUMBNAILS

#pragma endregion
