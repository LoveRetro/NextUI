#include "otamenu.hpp"

#include <zip.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

using namespace OTA;

#define OTA_REPO "LoveRetro/NextUI"
#define OTA_API "https://api.github.com/repos/" OTA_REPO

// Shown when picking anything but the newest release. Same wording as the standalone updater.
#define OTA_DOWNGRADE_WARNING                                  \
    "WARNING\n\n"                                              \
    "Downgrades are not fully\nsupported by NextUI!\n\n"        \
    "Settings may be lost or unstable\nin old versions, and manual\nediting of settings or files may be required"

namespace
{
    ///////////////////////////////////////////////////////////
    // Minimal JSON reader, just enough for the GitHub API.

    struct Json
    {
        enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
        double num = 0;
        std::string str;
        std::vector<Json> arr;
        std::vector<std::pair<std::string, Json>> obj;

        const Json *get(const char *key) const
        {
            for (const auto &kv : obj)
                if (kv.first == key)
                    return &kv.second;
            return nullptr;
        }
        std::string string(const char *key) const
        {
            auto v = get(key);
            return (v && v->type == Str) ? v->str : std::string();
        }
        uint64_t number(const char *key) const
        {
            auto v = get(key);
            return (v && v->type == Num && v->num > 0) ? (uint64_t)v->num : 0;
        }
    };

    class JsonParser
    {
        const char *p, *end;

        void ws()
        {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
                p++;
        }

        static void utf8(std::string &out, uint32_t cp)
        {
            if (cp < 0x80)
                out += (char)cp;
            else if (cp < 0x800)
            {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
            }
            else if (cp < 0x10000)
            {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
            else
            {
                out += (char)(0xF0 | (cp >> 18));
                out += (char)(0x80 | ((cp >> 12) & 0x3F));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
        }

        bool hex4(uint32_t &out)
        {
            if (end - p < 4)
                return false;
            out = 0;
            for (int i = 0; i < 4; i++, p++)
            {
                if (!isxdigit((unsigned char)*p))
                    return false;
                out = out * 16 + (isdigit((unsigned char)*p) ? *p - '0' : (tolower(*p) - 'a' + 10));
            }
            return true;
        }

        bool string(std::string &out)
        {
            if (p >= end || *p != '"')
                return false;
            p++;
            while (p < end && *p != '"')
            {
                if (*p != '\\')
                {
                    out += *p++;
                    continue;
                }
                if (++p >= end)
                    return false;
                switch (*p++)
                {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u':
                {
                    uint32_t cp;
                    if (!hex4(cp))
                        return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u')
                    {
                        p += 2;
                        uint32_t lo;
                        if (!hex4(lo))
                            return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: out += p[-1]; break; // " \ /
                }
            }
            if (p >= end)
                return false;
            p++;
            return true;
        }

        bool literal(const char *word)
        {
            size_t n = strlen(word);
            if ((size_t)(end - p) < n || strncmp(p, word, n) != 0)
                return false;
            p += n;
            return true;
        }

    public:
        JsonParser(const std::string &s) : p(s.data()), end(s.data() + s.size()) {}

        bool value(Json &out, int depth = 0)
        {
            if (depth > 32)
                return false;
            ws();
            if (p >= end)
                return false;
            switch (*p)
            {
            case '{':
            {
                out.type = Json::Obj;
                p++;
                ws();
                if (p < end && *p == '}')
                    return ++p, true;
                while (true)
                {
                    ws();
                    std::string key;
                    if (!string(key))
                        return false;
                    ws();
                    if (p >= end || *p++ != ':')
                        return false;
                    out.obj.emplace_back(std::move(key), Json());
                    if (!value(out.obj.back().second, depth + 1))
                        return false;
                    ws();
                    if (p >= end)
                        return false;
                    if (*p == ',')
                    {
                        p++;
                        continue;
                    }
                    return *p++ == '}';
                }
            }
            case '[':
            {
                out.type = Json::Arr;
                p++;
                ws();
                if (p < end && *p == ']')
                    return ++p, true;
                while (true)
                {
                    out.arr.emplace_back();
                    if (!value(out.arr.back(), depth + 1))
                        return false;
                    ws();
                    if (p >= end)
                        return false;
                    if (*p == ',')
                    {
                        p++;
                        continue;
                    }
                    return *p++ == ']';
                }
            }
            case '"':
                out.type = Json::Str;
                return string(out.str);
            case 't':
                out.type = Json::Bool;
                return literal("true");
            case 'f':
                out.type = Json::Bool;
                return literal("false");
            case 'n':
                out.type = Json::Null;
                return literal("null");
            default:
            {
                char *e = nullptr;
                out.type = Json::Num;
                out.num = strtod(p, &e);
                if (e == p)
                    return false;
                p = e;
                return true;
            }
            }
        }
    };

    ///////////////////////////////////////////////////////////
    // Helpers

    bool startsWith(const std::string &s, const std::string &prefix)
    {
        return s.compare(0, prefix.size(), prefix) == 0;
    }

    bool endsWith(const std::string &s, const std::string &suffix)
    {
        return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    std::string lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    // Release notes are GitHub flavoured markdown. Reduce them to plain text for the
    // viewer: keep the "What's Changed" list, drop links, formatting and emoji.
    std::string cleanNotes(std::string md)
    {
        md.erase(std::remove(md.begin(), md.end(), '\r'), md.end());

        // the part before the changelog is boilerplate (install links)
        size_t changed = md.find("## What's Changed");
        if (changed != std::string::npos)
            md = md.substr(changed);

        md = std::regex_replace(md, std::regex("\\[([^\\]]*)\\]\\([^)]*\\)"), "$1"); // [text](url)
        md = std::regex_replace(md, std::regex("\\s+in\\s+https?://\\S+"), "");          // " in <pr url>"
        md = std::regex_replace(md, std::regex("https?://\\S+"), "");
        md = std::regex_replace(md, std::regex("(\\*\\*|__|`)"), "");

        std::string out, line;
        std::istringstream in(md);
        int blanks = 0;
        while (std::getline(in, line))
        {
            if (startsWith(line, "Full Changelog") || line.find("Full Changelog:") != std::string::npos)
                continue;
            line = std::regex_replace(line, std::regex("^#+\\s*"), "");
            line = std::regex_replace(line, std::regex("^\\s*[*-]\\s+"), "- ");
            // the UI font has no emoji, drop 4 byte sequences, joiners and variation selectors
            std::string clean;
            for (size_t i = 0; i < line.size(); i++)
            {
                unsigned char c = line[i];
                if (c >= 0xF0)
                {
                    i += 3;
                    continue;
                }
                if (line.compare(i, 3, "\xE2\x80\x8D") == 0 || line.compare(i, 3, "\xEF\xB8\x8F") == 0)
                {
                    i += 2;
                    continue;
                }
                clean += line[i];
            }
            line = clean;
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (line.empty())
            {
                if (!out.empty() && ++blanks == 1)
                    out += "\n";
                continue;
            }
            blanks = 0;
            out += line + "\n";
        }
        while (!out.empty() && out.back() == '\n')
            out.pop_back();
        return out;
    }

    std::string formatMB(uint64_t bytes)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f", bytes / (1024.0 * 1024.0));
        return buf;
    }

    // Where updates get staged. Overridable so the flow can be exercised without
    // touching the real card contents (and without rebooting).
    const char *stagingRoot()
    {
        const char *root = getenv("NEXTUI_OTA_ROOT");
        return (root && *root) ? root : SDCARD_PATH;
    }

    bool mkdirs(const std::string &path)
    {
        std::string cur;
        for (size_t i = 0; i <= path.size(); i++)
        {
            if (i == path.size() || path[i] == '/')
            {
                if (!cur.empty() && mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
                    return false;
            }
            if (i < path.size())
                cur += path[i];
        }
        return true;
    }

    void removeTree(const std::string &path)
    {
        std::string cmd = "rm -rf '" + path + "'";
        if (system(cmd.c_str()) != 0)
            LOG_error("OTA: failed to remove %s\n", path.c_str());
    }

    // What the running install looks like: release name on line 1, short commit sha on line 2.
    void readInstalledVersion(std::string &name, std::string &sha)
    {
        std::ifstream f(ROOT_SYSTEM_PATH "/version.txt");
        std::getline(f, name);
        std::getline(f, sha);
    }

    // Run curl in a child process, writing the response to `dest`. `tick` is polled
    // while it runs and returns true to cancel. A non-zero `maxSecs` is a hard limit on
    // top of curl's own timeouts. Returns an empty string on success, kCancelled if
    // cancelled, otherwise a user facing error.
    //
    // The child is started with posix_spawn rather than fork: this process is
    // multithreaded and large, and a forked child would inherit our signal handlers,
    // so it could not be reliably killed before it execs.
    const char *const kCancelled = "Cancelled";

    void killAndReap(pid_t pid)
    {
        kill(pid, SIGKILL);
        // bounded, never block the UI on a child that refuses to die
        for (int i = 0; i < 100; i++)
        {
            int status;
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid || r < 0)
                return;
            usleep(10 * 1000);
        }
        LOG_error("OTA: curl (pid %d) did not exit\n", (int)pid);
    }

    std::string curlToFile(const std::string &url, const std::string &dest,
                           const std::vector<std::string> &extraArgs,
                           const std::function<bool()> &tick, unsigned pollUs, unsigned maxSecs = 0,
                           int *curlExit = nullptr)
    {
        // HTTP/1.1 only, HTTP/2 stream errors (curl exit 92) were seen on retries
        std::vector<std::string> args = {"curl", "-f", "-s", "-k", "-L", "--http1.1", "--connect-timeout", "15",
                                         "-A", "NextUI", "-o", dest, url};
        args.insert(args.begin() + 1, extraArgs.begin(), extraArgs.end());
        std::vector<char *> argv;
        for (auto &a : args)
            argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawnattr_t attr;
        posix_spawn_file_actions_init(&actions);
        posix_spawnattr_init(&attr);
        // quiet, curl errors surface through the exit code
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        // default signal handling and an empty mask, whatever we have installed
        sigset_t all, none;
        sigfillset(&all);
        sigemptyset(&none);
        posix_spawnattr_setsigdefault(&attr, &all);
        posix_spawnattr_setsigmask(&attr, &none);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);

        pid_t pid = 0;
        int rc = posix_spawnp(&pid, "curl", &actions, &attr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attr);
        if (rc != 0)
            return "Could not start curl";

        const time_t started = time(nullptr);
        int status = 0;
        while (true)
        {
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid)
                break;
            if (r < 0)
                return "Request failed";
            if (tick())
            {
                killAndReap(pid);
                unlink(dest.c_str());
                return kCancelled;
            }
            if (maxSecs && (unsigned)(time(nullptr) - started) > maxSecs)
            {
                killAndReap(pid);
                unlink(dest.c_str());
                return "The request timed out";
            }
            if (pollUs)
                usleep(pollUs);
        }

        if (curlExit)
            *curlExit = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            return "";
        unlink(dest.c_str());
        if (!WIFEXITED(status))
            return "Request aborted";
        switch (WEXITSTATUS(status))
        {
        case 6:
        case 7: return "Could not reach the server";
        case 22: return "The server returned an error\n(GitHub rate limit?)";
        case 28: return "The request timed out";
        default: return "Request failed (curl exit " + std::to_string(WEXITSTATUS(status)) + ")";
        }
    }

    std::string fetch(const std::string &url, std::string &out, const std::function<bool()> &tick)
    {
        const std::string tmp = "/tmp/nextui-ota-fetch.json";
        std::string err;
        // one automatic retry for connection level hiccups (reset, partial transfer, protocol errors)
        for (int attempt = 0; attempt < 2; attempt++)
        {
            int code = 0;
            err = curlToFile(url, tmp, {"--max-time", "30"}, tick, 0, 45, &code);
            bool transient = code == 18 || code == 52 || code == 55 || code == 56 || code == 92;
            if (err.empty() || !transient)
                break;
        }
        if (!err.empty())
            return err;
        std::ifstream f(tmp, std::ios::binary);
        std::stringstream buf;
        buf << f.rdbuf();
        out = buf.str();
        unlink(tmp.c_str());
        return out.empty() ? "Empty response" : "";
    }

    const Asset *pickAsset(const Release &release, bool full)
    {
        const char *kind = full ? "all" : "base";
        for (const auto &a : release.assets)
            if (endsWith(lower(a.name), std::string("-") + kind + ".zip"))
                return &a;
        for (const auto &a : release.assets)
            if (lower(a.name).find(kind) != std::string::npos)
                return &a;
        return release.assets.empty() ? nullptr : &release.assets.front();
    }

    // Modal A/B prompt, returns true on A.
    bool confirm(const std::string &message)
    {
        MenuList::showOverlay(message, OverlayDismissMode::AcceptOrReturn);
        bool accepted = false;
        while (true)
        {
            GFX_startFrame();
            PAD_poll();
            if (PAD_justPressed(BTN_A))
            {
                accepted = true;
                break;
            }
            if (PAD_justPressed(BTN_B))
                break;
            GFX_sync();
        }
        MenuList::hideOverlay();
        return accepted;
    }

    ///////////////////////////////////////////////////////////
    // Install staging

    // Everything in the base package that is user content rather than part of the
    // boot payload. Base updates only stage what is left (MinUI.zip, boot folders).
    bool isUserContent(const std::string &name)
    {
        static const std::set<std::string> roots = {
            "Bios", "Roms", "Saves", "Shaders", "Overlays", "Emus", "Cheats", "Collections", "Tools", "README.txt"};
        std::string first = name.substr(0, name.find('/'));
        return roots.count(first) || endsWith(lower(first), ".pakz");
    }

    // Full updates unpack everything except Tools (that is carried by MinUI.zip, and
    // we are running out of it) and Roms folders for systems the user already has.
    class FullFilter
    {
        std::map<std::string, bool> decided;
        std::regex tagRe{"\\((\\w+)\\)"};

        bool romFolderExists(const std::string &folder)
        {
            std::smatch m;
            if (!std::regex_search(folder, m, tagRe))
                return false;
            std::string needle = "(" + m[1].str() + ")";
            std::string romsPath = std::string(stagingRoot()) + "/Roms";
            DIR *d = opendir(romsPath.c_str());
            if (!d)
                return false;
            bool found = false;
            while (auto *e = readdir(d))
            {
                if (std::string(e->d_name).find(needle) != std::string::npos)
                {
                    found = true;
                    break;
                }
            }
            closedir(d);
            return found;
        }

    public:
        bool operator()(const std::string &name)
        {
            if (startsWith(name, "Tools/") || name == "Tools")
                return false;
            if (startsWith(name, "Roms/"))
            {
                std::string folder = name.substr(5, name.find('/', 5) == std::string::npos ? std::string::npos : name.find('/', 5) - 5);
                if (folder.empty())
                    return true;
                auto it = decided.find(folder);
                if (it == decided.end())
                    it = decided.emplace(folder, !romFolderExists(folder)).first;
                return it->second;
            }
            return true;
        }
    };

    bool safeEntryName(const std::string &name)
    {
        if (name.empty() || name[0] == '/' || name.find('\\') != std::string::npos)
            return false;
        size_t pos = 0;
        while (pos <= name.size())
        {
            size_t next = name.find('/', pos);
            std::string part = name.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
            if (part == "..")
                return false;
            if (next == std::string::npos)
                break;
            pos = next + 1;
        }
        return true;
    }

    // Shared between the update thread and the UI loop.
    struct Progress
    {
        std::mutex lock;
        std::string message;
        std::atomic<bool> cancel{false};
        std::atomic<bool> cancellable{true};

        void set(const std::string &msg)
        {
            std::lock_guard<std::mutex> l(lock);
            message = msg;
        }
        std::string get()
        {
            std::lock_guard<std::mutex> l(lock);
            return message;
        }
    };

    // Returns an empty string on success.
    std::string download(const Asset &asset, const std::string &dest, Progress &progress)
    {
        auto tick = [&]() {
            struct stat st;
            uint64_t got = (stat(dest.c_str(), &st) == 0) ? (uint64_t)st.st_size : 0;
            char msg[256];
            if (asset.size > 0)
                snprintf(msg, sizeof(msg), "Downloading %s\n\n%d%%  (%s / %s MB)",
                         asset.name.c_str(), (int)(got * 100 / asset.size),
                         formatMB(got).c_str(), formatMB(asset.size).c_str());
            else
                snprintf(msg, sizeof(msg), "Downloading %s\n\n%s MB", asset.name.c_str(), formatMB(got).c_str());
            progress.set(msg);
            return progress.cancel.load();
        };
        std::string err = curlToFile(asset.url, dest, {"--speed-limit", "1024", "--speed-time", "30"}, tick, 250 * 1000);
        if (!err.empty())
            return err;

        struct stat st;
        if (stat(dest.c_str(), &st) != 0)
            return "Download failed";
        if (asset.size > 0 && (uint64_t)st.st_size != asset.size)
            return "Download incomplete";
        return "";
    }

    struct Entry
    {
        zip_uint64_t index;
        std::string name;
        zip_uint64_t size;
        bool isDir;
    };

    std::string extract(const std::string &zipPath, bool full, const std::string &root, Progress &progress)
    {
        int zerr = 0;
        zip_t *za = zip_open(zipPath.c_str(), ZIP_RDONLY, &zerr);
        if (!za)
            return "Could not open update package";

        FullFilter fullFilter;
        std::vector<Entry> entries;
        uint64_t total = 0;
        zip_int64_t count = zip_get_num_entries(za, 0);
        for (zip_int64_t i = 0; i < count; i++)
        {
            zip_stat_t st;
            if (zip_stat_index(za, i, 0, &st) != 0 || !(st.valid & ZIP_STAT_NAME))
                continue;
            std::string name = st.name;
            if (!safeEntryName(name))
                continue;
            if (full ? !fullFilter(name) : isUserContent(name))
                continue;
            bool isDir = name.back() == '/';
            zip_uint64_t size = (st.valid & ZIP_STAT_SIZE) ? st.size : 0;
            entries.push_back({(zip_uint64_t)i, name, size, isDir});
            total += size;
        }
        if (entries.empty())
        {
            zip_close(za);
            return "Update package has no usable content";
        }

        // compressed packages (cores) can grow a lot, so check against the real size
        struct statvfs vfs;
        if (statvfs(root.c_str(), &vfs) == 0)
        {
            uint64_t avail = (uint64_t)vfs.f_bavail * vfs.f_frsize;
            if (avail < total + 32 * 1024 * 1024)
            {
                zip_close(za);
                return "Not enough free space\n(need " + formatMB(total) + " MB)";
            }
        }

        // MinUI.zip is what the boot installer keys off, so it goes in last.
        std::stable_partition(entries.begin(), entries.end(), [](const Entry &e) { return e.name != "MinUI.zip"; });

        progress.cancellable = false;
        uint64_t done = 0;
        int lastPct = -1;
        std::vector<char> buf(64 * 1024);

        for (const auto &e : entries)
        {
            std::string dest = root + "/" + e.name;
            if (e.isDir)
            {
                mkdirs(dest);
                continue;
            }
            mkdirs(dest.substr(0, dest.rfind('/')));

            zip_file_t *zf = zip_fopen_index(za, e.index, 0);
            if (!zf)
            {
                zip_close(za);
                return "Could not read " + e.name;
            }
            // write beside the target and rename, so an interrupted run never leaves a truncated file
            std::string tmp = dest + ".part";
            FILE *out = fopen(tmp.c_str(), "wb");
            if (!out)
            {
                zip_fclose(zf);
                zip_close(za);
                return "Could not write " + e.name;
            }
            bool ok = true;
            zip_int64_t n;
            while ((n = zip_fread(zf, buf.data(), buf.size())) > 0)
            {
                if (fwrite(buf.data(), 1, n, out) != (size_t)n)
                {
                    ok = false;
                    break;
                }
                done += n;
                int pct = total ? (int)(done * 100 / total) : 100;
                if (pct != lastPct)
                {
                    lastPct = pct;
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Extracting update\n\n%d%%\n\nPlease wait...", pct);
                    progress.set(msg);
                }
            }
            ok = ok && n == 0;
            ok = (fclose(out) == 0) && ok;
            zip_fclose(zf);
            if (!ok || rename(tmp.c_str(), dest.c_str()) != 0)
            {
                unlink(tmp.c_str());
                zip_close(za);
                return "Failed writing " + e.name + " (disk full?)";
            }
        }

        zip_close(za);
        return "";
    }

    // Download + extract. Returns an empty string on success.
    std::string runUpdate(const Asset &asset, bool full, Progress &progress)
    {
        std::string root = stagingRoot();
        std::string workDir = root + "/.ota";
        std::string zipPath = workDir + "/update.zip";

        removeTree(workDir);
        if (!mkdirs(workDir))
            return "Could not create staging folder";

        // package plus its extracted contents need to fit at the same time
        struct statvfs vfs;
        if (statvfs(root.c_str(), &vfs) == 0)
        {
            uint64_t avail = (uint64_t)vfs.f_bavail * vfs.f_frsize;
            if (asset.size > 0 && avail < asset.size * 2 + 32 * 1024 * 1024)
            {
                removeTree(workDir);
                return "Not enough free space\n(need about " + formatMB(asset.size * 2) + " MB)";
            }
        }

        std::string err = download(asset, zipPath, progress);
        if (err.empty())
        {
            progress.set("Extracting update\n\n0%\n\nPlease wait...");
            err = extract(zipPath, full, root, progress);
        }
        removeTree(workDir);
        if (err.empty())
            sync();
        return err;
    }
}

///////////////////////////////////////////////////////////

namespace OTA
{
    // Left/right walks through releases (left = older). Moving away from the
    // newest release requires accepting the downgrade warning once.
    class VersionItem : public AbstractMenuItem
    {
        Menu &menu;

    public:
        VersionItem(Menu &menu)
            : AbstractMenuItem(ListItemType::Generic, "Version", "", nullptr), menu(menu)
        {
            setHints({"L/R", "CHANGE", "X", "NOTES"});
        }

        const std::any getValue() const override { return getLabel(); }
        const std::string getLabel() const override
        {
            if (menu.releases.empty())
                return "-";
            return menu.releases[menu.selected].tag + (menu.selected == 0 ? " (latest)" : "");
        }

        InputReactionHint handleInput(int &dirty) override
        {
            if (PAD_justPressed(BTN_X))
            {
                menu.showNotes();
                dirty = 1;
                return NoOp;
            }

            int step = 0;
            if (PAD_justRepeated(BTN_LEFT))
                step = 1;
            else if (PAD_justRepeated(BTN_RIGHT))
                step = -1;
            if (!step)
                return Unhandled;

            int target = std::max(0, std::min((int)menu.releases.size() - 1, menu.selected + step));
            if (menu.releases.empty() || target == menu.selected)
                return NoOp;
            if (target != 0 && !menu.confirmDowngrade())
            {
                dirty = 1;
                return NoOp;
            }
            menu.selected = target;
            menu.updateStatus();
            dirty = 1;
            return NoOp;
        }
    };

    Menu::Menu() : MenuList(MenuItemType::Fixed, "OTA Update", {})
    {
        items.push_back(new StaticMenuItem(ListItemType::Generic, "Installed", "", [this]() -> std::any { return installedLabel(); }));
        versionItem = new VersionItem(*this);
        items.push_back(versionItem);
        items.push_back(new MenuItem(ListItemType::Button, "Update (Base)", "Update MinUI.zip only",
                                     [this](AbstractMenuItem &) { return startUpdate(false); }));
        items.push_back(new MenuItem(ListItemType::Button, "Update (Full)", "Extract full zip files (base + extras)",
                                     [this](AbstractMenuItem &) { return startUpdate(true); }));

        MenuList::performLayout((SDL_Rect){0, 0, FIXED_WIDTH, FIXED_HEIGHT});
        layout_called = false;
    }

    std::string Menu::installedLabel() const
    {
        if (installed >= 0)
            return releases[installed].tag;
        return installedName.empty() ? "Unknown" : installedName;
    }

    void Menu::showNotes()
    {
        if (releases.empty())
            return;
        const Release &release = releases[selected];
        if (release.notes.empty())
            MenuList::showOverlay("No release notes available\nfor " + release.tag, OverlayDismissMode::DismissOnA);
        else
            MenuList::showTextViewer(release.tag, release.notes);
    }

    // The Version row's description tells the user what the current selection means.
    void Menu::updateStatus()
    {
        std::string status;
        if (releases.empty())
            status = "";
        else if (installed == selected)
            status = selected == 0 ? "Up to date" : "Currently installed";
        else if (installed < 0)
            status = selected == 0 ? "Update available" : "Older release";
        else
            status = selected < installed ? "Update available" : "Downgrade";
        versionItem->setDesc(status);
    }

    bool Menu::refresh(std::string &error)
    {
        error.clear();
        if (!(desktopBuild() || PWR_isOnline()))
        {
            error = "WiFi is not connected.\nConnect to a network first.";
            return false;
        }

        ScopedOverlay overlay("Checking for updates...", OverlayDismissMode::CancelHint);
        auto cancelled = []() {
            GFX_startFrame();
            PAD_poll();
            bool b = PAD_justPressed(BTN_B);
            GFX_sync();
            return b;
        };

        std::string body, err;
        err = fetch(OTA_API "/releases?per_page=100", body, cancelled);
        if (err == kCancelled)
            return false;
        if (!err.empty())
        {
            error = "Fetching releases failed:\n" + err;
            return false;
        }

        Json root;
        if (!JsonParser(body).value(root) || root.type != Json::Arr)
        {
            error = "Fetching releases failed:\nunexpected response";
            return false;
        }

        std::vector<Release> found;
        for (const auto &r : root.arr)
        {
            Release rel;
            rel.tag = r.string("tag_name");
            rel.notes = cleanNotes(r.string("body"));
            if (rel.tag.empty())
                continue;
            if (auto assets = r.get("assets"))
            {
                for (const auto &a : assets->arr)
                {
                    Asset asset;
                    asset.name = a.string("name");
                    asset.url = a.string("browser_download_url");
                    asset.size = a.number("size");
                    if (!asset.name.empty() && !asset.url.empty())
                        rel.assets.push_back(std::move(asset));
                }
            }
            if (!rel.assets.empty())
                found.push_back(std::move(rel));
        }
        if (found.empty())
        {
            error = "No releases found";
            return false;
        }

        // Match the installed build against release tags by commit sha. Best
        // effort, a missing tag list only means we can't tell what is installed.
        std::string instName, instSha;
        readInstalledVersion(instName, instSha);
        int instIdx = -1;
        body.clear();
        err = fetch(OTA_API "/tags?per_page=100", body, cancelled);
        if (err == kCancelled)
            return false;
        if (err.empty())
        {
            Json tags;
            if (JsonParser(body).value(tags) && tags.type == Json::Arr)
            {
                for (const auto &t : tags.arr)
                {
                    std::string name = t.string("name");
                    auto commit = t.get("commit");
                    std::string sha = commit ? commit->string("sha") : "";
                    for (size_t i = 0; i < found.size(); i++)
                    {
                        if (found[i].tag != name)
                            continue;
                        found[i].sha = sha;
                        if (instIdx < 0 && !instSha.empty() && startsWith(sha, instSha))
                            instIdx = (int)i;
                    }
                }
            }
        }

        releases = std::move(found);
        installed = instIdx;
        installedName = instName;
        selected = 0;
        warned = false;
        updateStatus();
        return true;
    }

    bool Menu::confirmDowngrade()
    {
        if (!warned)
            warned = confirm(OTA_DOWNGRADE_WARNING);
        return warned;
    }

    InputReactionHint Menu::startUpdate(bool full)
    {
        if (releases.empty())
            return NoOp;
        if (!(desktopBuild() || PWR_isOnline()))
        {
            MenuList::showOverlay("WiFi is not connected.", OverlayDismissMode::DismissOnA);
            return NoOp;
        }

        const Release &release = releases[selected];
        const Asset *asset = pickAsset(release, full);
        if (!asset)
        {
            MenuList::showOverlay("No downloadable package found\nfor " + release.tag, OverlayDismissMode::DismissOnA);
            return NoOp;
        }

        Progress progress;
        progress.set("Preparing update...");
        std::string result;
        std::atomic<bool> done{false};

        PWR_disableSleep();
        std::thread worker([&]() {
            result = runUpdate(*asset, full, progress);
            done = true;
        });

        std::string shown;
        bool shownCancellable = true;
        MenuList::showOverlay(progress.get(), OverlayDismissMode::CancelHint);
        while (!done)
        {
            GFX_startFrame();
            PAD_poll();

            if (progress.cancellable && PAD_justPressed(BTN_B))
                progress.cancel = true;

            std::string current = progress.get();
            bool cancellable = progress.cancellable;
            if (current != shown || cancellable != shownCancellable)
            {
                shown = current;
                shownCancellable = cancellable;
                MenuList::showOverlay(shown, cancellable ? OverlayDismissMode::CancelHint : OverlayDismissMode::None);
            }
            GFX_sync();
        }
        worker.join();
        PWR_enableSleep();

        if (!result.empty())
        {
            if (result == kCancelled)
                MenuList::hideOverlay();
            else
                MenuList::showOverlay("Update failed:\n" + result, OverlayDismissMode::DismissOnA);
            return NoOp;
        }

        // The standalone updater pak is superseded by this menu, offer to clean it up
        const std::string oldUpdater = std::string(stagingRoot()) + "/Tools/" PLATFORM "/Updater.pak";
        struct stat st;
        if (stat(oldUpdater.c_str(), &st) == 0 &&
            confirm("The standalone Updater pak is\nno longer needed.\n\nRemove it?"))
            removeTree(oldUpdater);

        if (getenv("NEXTUI_OTA_ROOT") || desktopBuild())
        {
            // staged somewhere other than a real card root, nothing to install
            MenuList::showOverlay("Update staged in\n" + std::string(stagingRoot()), OverlayDismissMode::DismissOnA);
            return NoOp;
        }

        MenuList::showOverlay("Update complete,\nrebooting...", OverlayDismissMode::None);
        sleep(2);
        PWR_powerOff(1);
        return NoOp;
    }
}
