#include "ui/explorer/explorer_view.hpp"

#include "install/package_stream.hpp"

#include <cstdio>
#include <fstream>
#include <vector>

extern "C" {
#include "core/util.h"
}

namespace pipensx::ui {

bool ExplorerView::installLocalFile(const std::string& path,
                                    const std::string& name,
                                    install::InstallStorageTarget target,
                                    std::shared_ptr<std::atomic<bool>> alive,
                                    std::string& error) {
    const size_t dot = name.find_last_of('.');
    const std::string ext = dot == std::string::npos ? "" : name.substr(dot);
    auto ciEquals = [](const std::string& a, const char* b) {
        return strcasecmp(a.c_str(), b) == 0;
    };
    const bool isXci = ciEquals(ext, ".xci") || ciEquals(ext, ".xcz");
    const bool compressed = ciEquals(ext, ".nsz") || ciEquals(ext, ".xcz");

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "No se pudo abrir el archivo.";
        return false;
    }
    const std::streamoff totalSize = input.tellg();
    if (totalSize <= 0) {
        error = "El archivo está vacío.";
        return false;
    }
    input.seekg(0);

    std::unique_ptr<install::InstallBackend> backend =
        install::createInstallBackend("sdmc:/switch/freeshop-client", target);
    char taskId[48];
    std::snprintf(taskId, sizeof(taskId), "explorer-%llu",
                  static_cast<unsigned long long>(now_ms()));
    if (!backend->beginPackage(taskId, name)) {
        error = backend->error();
        return false;
    }

    install::InstallBackend* backendPtr = backend.get();
    install::PackageCallbacks callbacks;
    callbacks.beginFile = [backendPtr](const std::string& n, uint64_t s) {
        return backendPtr->beginFile(n, s);
    };
    callbacks.setFileSize = [backendPtr](uint64_t s) {
        return backendPtr->setFileSize(s);
    };
    callbacks.writeFile = [backendPtr](const uint8_t* d, size_t s) {
        return backendPtr->writeFile(d, s);
    };
    callbacks.endFile = [backendPtr] { return backendPtr->endFile(); };
    install::PackageStream stream(compressed, std::move(callbacks), taskId, isXci);

    // Heap, not a stack std::array - this runs on a brls::async() pool
    // thread, whose default stack is far smaller than the main thread's
    // (see file_explorer_service.cpp's copyFileContents for the same call).
    std::vector<char> buffer(256 * 1024);
    const uint64_t total = static_cast<uint64_t>(totalSize);
    uint64_t done = 0;
    uint64_t lastNotifyMs = 0;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = input.gcount();
        if (got <= 0)
            break;
        if (!stream.write(reinterpret_cast<const uint8_t*>(buffer.data()),
                          static_cast<size_t>(got))) {
            error = stream.error();
            backend->rollbackPackage();
            return false;
        }
        done += static_cast<uint64_t>(got);

        if (!alive->load()) {
            backend->rollbackPackage();
            error = "cancelado";
            return false;
        }
        const uint64_t nowMs = now_ms();
        if (nowMs - lastNotifyMs >= 200 || done >= total) {
            lastNotifyMs = nowMs;
            brls::sync([this, alive, name, done, total] {
                if (alive->load())
                    updateInstallProgress(name, done, total);
            });
        }
    }
    if (input.bad()) {
        error = "No se pudo leer el archivo.";
        backend->rollbackPackage();
        return false;
    }
    if (!stream.finish()) {
        error = stream.error();
        backend->rollbackPackage();
        return false;
    }
    bool alreadyInstalled = false;
    if (!backend->commitPackage(alreadyInstalled)) {
        error = backend->error();
        return false;
    }
    return true;
}

int ExplorerDataSource::numberOfRows(brls::RecyclerFrame*, int) {
    return static_cast<int>(owner_->entries().size());
}

brls::RecyclerCell* ExplorerDataSource::cellForRow(
    brls::RecyclerFrame* recycler, brls::IndexPath index) {
    auto* cell = static_cast<ExplorerCell*>(
        recycler->dequeueReusableCell("Explorer"));
    cell->setEntry(owner_->entries()[index.row]);
    return cell;
}

void ExplorerDataSource::didSelectRowAt(brls::RecyclerFrame*,
                                        brls::IndexPath index) {
    owner_->select(static_cast<size_t>(index.row));
}

}  // namespace pipensx::ui
