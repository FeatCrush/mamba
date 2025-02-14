// Copyright (c) 2019, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include "mamba/core/mamba_fs.hpp"
#include "mamba/core/output.hpp"
#include "mamba/core/package_cache.hpp"
#include "mamba/core/subdirdata.hpp"
#include "mamba/core/url.hpp"
#include <fcntl.h>
#include <sys/stat.h>

namespace decompress
{
    bool raw(const std::string& in, const std::string& out)
    {
        int r;
        std::ptrdiff_t size;

        LOG_INFO << "Decompressing from " << in << " to " << out;

        struct archive* a = archive_read_new();
        archive_read_support_filter_bzip2(a);
        archive_read_support_format_raw(a);
        // TODO figure out good value for this
        const std::size_t BLOCKSIZE = 16384;
        r = archive_read_open_filename(a, in.c_str(), BLOCKSIZE);
        if (r != ARCHIVE_OK)
        {
            return false;
        }

        struct archive_entry* entry;
        std::ofstream out_file(out);
        char buff[BLOCKSIZE];
        std::size_t buffsize = BLOCKSIZE;
        r = archive_read_next_header(a, &entry);
        if (r != ARCHIVE_OK)
        {
            return false;
        }

        while (true)
        {
            size = archive_read_data(a, &buff, buffsize);
            if (size < ARCHIVE_OK)
            {
                throw std::runtime_error(std::string("Could not read archive: ")
                                         + archive_error_string(a));
            }
            if (size == 0)
            {
                break;
            }
            out_file.write(buff, size);
        }

        archive_read_free(a);
        return true;
    }
}  // namespace decompress

namespace mamba
{
    MSubdirData::MSubdirData(const std::string& name,
                             const std::string& repodata_url,
                             const std::string& repodata_fn,
                             bool is_noarch)
        : m_loaded(false)
        , m_download_complete(false)
        , m_repodata_url(repodata_url)
        , m_name(name)
        , m_json_fn(repodata_fn)
        , m_solv_fn(repodata_fn.substr(0, repodata_fn.size() - 4) + "solv")
        , m_is_noarch(is_noarch)
    {
    }

    fs::file_time_type::duration MSubdirData::check_cache(
        const fs::path& cache_file, const fs::file_time_type::clock::time_point& ref)
    {
        try
        {
            auto last_write = fs::last_write_time(cache_file);
            auto tdiff = ref - last_write;
            return tdiff;
        }
        catch (...)
        {
            // could not open the file...
            return fs::file_time_type::duration::max();
        }
    }

    bool MSubdirData::loaded()
    {
        return m_loaded;
    }

    bool MSubdirData::forbid_cache()
    {
        return starts_with(m_repodata_url, "file://");
    }

    bool MSubdirData::load()
    {
        auto now = fs::file_time_type::clock::now();
        auto cache_age = check_cache(m_json_fn, now);
        if (cache_age != fs::file_time_type::duration::max() && !forbid_cache())
        {
            LOG_INFO << "Found valid cache file.";
            m_mod_etag = read_mod_and_etag();
            if (m_mod_etag.size() != 0)
            {
                int max_age = 0;
                if (Context::instance().local_repodata_ttl > 1)
                {
                    max_age = Context::instance().local_repodata_ttl;
                }
                else if (Context::instance().local_repodata_ttl == 1)
                {
                    // TODO error handling if _cache_control key does not exist!
                    auto el = m_mod_etag.value("_cache_control", std::string(""));
                    max_age = get_cache_control_max_age(el);
                }

                auto cache_age_seconds
                    = std::chrono::duration_cast<std::chrono::seconds>(cache_age).count();
                if ((max_age > cache_age_seconds || Context::instance().offline))
                {
                    // cache valid!
                    LOG_INFO << "Using cache " << m_repodata_url
                             << " age in seconds: " << cache_age_seconds << " / " << max_age;
                    std::string prefix = m_name;
                    prefix.resize(PREFIX_LENGTH - 1, ' ');
                    Console::stream() << prefix << " Using cache";

                    m_loaded = true;
                    m_json_cache_valid = true;

                    // check solv cache
                    auto solv_age = check_cache(m_solv_fn, now);
                    LOG_INFO << "Solv cache age in seconds: "
                             << std::chrono::duration_cast<std::chrono::seconds>(solv_age).count();
                    if (solv_age != fs::file_time_type::duration::max()
                        && solv_age.count() <= cache_age.count())
                    {
                        LOG_INFO << "Also using .solv cache file";
                        m_solv_cache_valid = true;
                    }
                    return true;
                }
            }
            else
            {
                LOG_INFO << "Could not determine cache file mod / etag headers";
            }
            create_target(m_mod_etag);
        }
        else
        {
            LOG_INFO << "No cache found " << m_repodata_url;
            if (!Context::instance().offline || forbid_cache())
            {
                create_target(m_mod_etag);
            }
        }
        return true;
    }

    std::string MSubdirData::cache_path() const
    {
        // TODO invalidate solv cache on version updates!!
        if (m_json_cache_valid && m_solv_cache_valid)
        {
            return m_solv_fn;
        }
        else if (m_json_cache_valid)
        {
            return m_json_fn;
        }
        throw std::runtime_error("Cache not loaded!");
    }

    DownloadTarget* MSubdirData::target()
    {
        return m_target.get();
    }

    const std::string& MSubdirData::name() const
    {
        return m_name;
    }

    bool MSubdirData::finalize_transfer()
    {
        if (m_target->result != 0 || m_target->http_status >= 400)
        {
            LOG_INFO << "Unable to retrieve repodata (response: " << m_target->http_status
                     << ") for " << m_repodata_url;
            m_progress_bar.set_postfix(std::to_string(m_target->http_status) + " Failed");
            m_progress_bar.set_full();
            m_progress_bar.mark_as_completed();
            m_loaded = false;
            return false;
        }

        LOG_INFO << "HTTP response code: " << m_target->http_status;
        // Note HTTP status == 0 for files
        if (m_target->http_status == 0 || m_target->http_status == 200
            || m_target->http_status == 304)
        {
            m_download_complete = true;
        }
        else
        {
            LOG_WARNING << "HTTP response code indicates error, retrying.";
            throw std::runtime_error("Unhandled HTTP code: "
                                     + std::to_string(m_target->http_status));
        }

        if (m_target->http_status == 304)
        {
            // cache still valid
            auto now = fs::file_time_type::clock::now();
            auto cache_age = check_cache(m_json_fn, now);
            auto solv_age = check_cache(m_solv_fn, now);

            utimensat(AT_FDCWD, m_json_fn.c_str(), NULL, AT_SYMLINK_NOFOLLOW);
            LOG_INFO << "Solv age: "
                     << std::chrono::duration_cast<std::chrono::seconds>(solv_age).count()
                     << ", JSON age: "
                     << std::chrono::duration_cast<std::chrono::seconds>(cache_age).count();
            if (solv_age != fs::file_time_type::duration::max()
                && solv_age.count() <= cache_age.count())
            {
                utimensat(AT_FDCWD, m_solv_fn.c_str(), NULL, AT_SYMLINK_NOFOLLOW);
                m_solv_cache_valid = true;
            }

            m_progress_bar.set_postfix("No change");
            m_progress_bar.set_full();
            m_progress_bar.mark_as_completed();

            m_json_cache_valid = true;
            m_loaded = true;
            m_temp_file.reset(nullptr);
            return true;
        }

        LOG_INFO << "Finalized transfer: " << m_repodata_url;

        m_mod_etag.clear();
        m_mod_etag["_url"] = m_repodata_url;
        m_mod_etag["_etag"] = m_target->etag;
        m_mod_etag["_mod"] = m_target->mod;
        m_mod_etag["_cache_control"] = m_target->cache_control;

        LOG_INFO << "Opening: " << m_json_fn;
        std::ofstream final_file(m_json_fn);

        create_cache_dir();

        if (!final_file.is_open())
        {
            LOG_ERROR << "Could not open file " << m_json_fn;
            exit(1);
        }

        if (ends_with(m_repodata_url, ".bz2"))
        {
            m_progress_bar.set_postfix("Decomp...");
            decompress();
        }

        m_progress_bar.set_postfix("Finalizing...");

        std::ifstream temp_file(m_temp_file->path());
        std::stringstream temp_json;
        temp_json << m_mod_etag.dump();

        // replace `}` with `,`
        temp_json.seekp(-1, temp_json.cur);
        temp_json << ',';
        final_file << temp_json.str();
        temp_file.seekg(1);
        std::copy(std::istreambuf_iterator<char>(temp_file),
                  std::istreambuf_iterator<char>(),
                  std::ostreambuf_iterator<char>(final_file));

        if (!temp_file)
        {
            LOG_ERROR << "Could not write out repodata file '" << m_json_fn
                      << "': " << strerror(errno);
            fs::remove(m_json_fn);
            exit(1);
        }

        m_progress_bar.set_postfix("Done");
        m_progress_bar.set_full();
        m_progress_bar.mark_as_completed();

        m_json_cache_valid = true;
        m_loaded = true;

        temp_file.close();
        m_temp_file.reset(nullptr);
        final_file.close();

        utimensat(AT_FDCWD, m_json_fn.c_str(), NULL, AT_SYMLINK_NOFOLLOW);

        return true;
    }

    bool MSubdirData::decompress()
    {
        LOG_INFO << "Decompressing metadata";
        auto json_temp_file = std::make_unique<TemporaryFile>();
        bool result = decompress::raw(m_temp_file->path(), json_temp_file->path());
        if (!result)
        {
            LOG_WARNING << "Could not decompress " << m_temp_file->path();
        }
        std::swap(json_temp_file, m_temp_file);
        return result;
    }

    void MSubdirData::create_target(nlohmann::json& mod_etag)
    {
        m_temp_file = std::make_unique<TemporaryFile>();
        m_progress_bar = Console::instance().add_progress_bar(m_name);
        m_target = std::make_unique<DownloadTarget>(m_name, m_repodata_url, m_temp_file->path());
        m_target->set_progress_bar(m_progress_bar);
        // if we get something _other_ than the noarch, we DO NOT throw if the file
        // can't be retrieved
        if (!m_is_noarch)
        {
            m_target->set_ignore_failure(true);
        }
        m_target->set_finalize_callback(&MSubdirData::finalize_transfer, this);
        m_target->set_mod_etag_headers(mod_etag);
    }

    std::size_t MSubdirData::get_cache_control_max_age(const std::string& val)
    {
        static std::regex max_age_re("max-age=(\\d+)");
        std::smatch max_age_match;
        bool matches = std::regex_search(val, max_age_match, max_age_re);
        if (!matches)
            return 0;
        return std::stoi(max_age_match[1]);
    }

    nlohmann::json MSubdirData::read_mod_and_etag()
    {
        // parse json at the beginning of the stream such as
        // {"_url": "https://conda.anaconda.org/conda-forge/linux-64",
        // "_etag": "W/\"6092e6a2b6cec6ea5aade4e177c3edda-8\"",
        // "_mod": "Sat, 04 Apr 2020 03:29:49 GMT",
        // "_cache_control": "public, max-age=1200"

        auto extract_subjson = [](std::ifstream& s) {
            char next;
            std::string result;
            bool escaped = false;
            int i = 0, N = 4;
            while (s.get(next))
            {
                if (next == '"')
                {
                    if (!escaped)
                    {
                        i++;
                    }
                    else
                    {
                        escaped = false;
                    }
                    // 4 keys == 4 ticks
                    if (i == 4 * N)
                    {
                        return result + "\"}";
                    }
                }
                else if (next == '\\')
                {
                    escaped = true;
                }
                result.push_back(next);
            }
            return std::string();
        };

        std::ifstream in_file(m_json_fn);
        auto json = extract_subjson(in_file);
        nlohmann::json result;
        try
        {
            result = nlohmann::json::parse(json);
            return result;
        }
        catch (...)
        {
            LOG_WARNING << "Could not parse mod / etag header!";
            return nlohmann::json();
        }
    }

    std::string cache_fn_url(const std::string& url)
    {
        return cache_name_from_url(url) + ".json";
    }

    std::string create_cache_dir()
    {
        std::string cache_dir
            = PackageCacheData::first_writable().get_pkgs_dir().string() + "/cache";
        fs::create_directories(cache_dir);
#ifndef _WIN32
        ::chmod(cache_dir.c_str(), 02775);
#endif
        return cache_dir;
    }

    MRepo MSubdirData::create_repo(MPool& pool)
    {
        RepoMetadata meta{ m_repodata_url,
                           Context::instance().add_pip_as_python_dependency,
                           m_mod_etag["_etag"],
                           m_mod_etag["_mod"] };

        return MRepo(pool, m_name, cache_path(), meta);
    }

    void MSubdirData::clear_cache()
    {
        if (fs::exists(m_json_fn))
        {
            fs::remove(m_json_fn);
        }
        if (fs::exists(m_solv_fn))
        {
            fs::remove(m_solv_fn);
        }
    }
}  // namespace mamba
