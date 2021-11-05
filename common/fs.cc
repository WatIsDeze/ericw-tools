/*
    Copyright (C) 1996-1997  Id Software, Inc.
    Copyright (C) 1997       Greg Lewis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    See file, 'COPYING', for details.
*/

#include "common/cmdlib.hh"
#include "common/fs.hh"
#include <fstream>
#include <memory>
#include <array>
#include <set>

namespace fs
{
    path qdir, gamedir, basedir;

    struct directory_archive : archive_like
    {
        using archive_like::archive_like;
        
        virtual data load(const path &filename) override
        {
            path p = pathname / filename;

            if (!exists(p)) {
                return std::nullopt;
            }

            uintmax_t size = file_size(p);
            std::ifstream stream(pathname / filename, std::ios_base::in | std::ios_base::binary);
            std::vector<uint8_t> data(size);
            stream.read(reinterpret_cast<char *>(data.data()), size);
            return data;
        }
    };

    struct pak_archive : archive_like
    {
        std::ifstream pakstream;

        struct pak_header
        {
            std::array<char, 4>     magic;
            uint32_t                offset;
            uint32_t                size;

            auto stream_data()
            {
                return std::tie(magic, offset, size);
            }
        };

        struct pak_file
        {
            std::array<char, 56>    name;
            uint32_t                offset;
            uint32_t                size;

            auto stream_data()
            {
                return std::tie(name, offset, size);
            }
        };

        std::unordered_map<std::string, std::tuple<uint32_t, uint32_t>, case_insensitive_hash, case_insensitive_equal> files;

        inline pak_archive(const path &pathname) :
            archive_like(pathname),
            pakstream(pathname, std::ios_base::in | std::ios_base::binary)
        {
            pakstream >> endianness<std::endian::little>;

            pak_header header;

            pakstream >= header;

            if (header.magic != std::array<char, 4> { 'P', 'A', 'C', 'K' }) {
                throw std::exception("Bad magic");
            }

            size_t totalFiles = header.size / sizeof(pak_file);

            files.reserve(totalFiles);

            pakstream.seekg(header.offset);

            for (size_t i = 0; i < totalFiles; i++) {
                pak_file file;

                pakstream >= file;

                files[file.name.data()] = std::make_tuple(file.offset, file.size);
            }
        }
        
        virtual data load(const path &filename) override
        {
            auto it = files.find(filename.generic_string());

            if (it == files.end()) {
                return std::nullopt;
            }

            pakstream.seekg(std::get<0>(it->second));
            uintmax_t size = std::get<1>(it->second);
            std::vector<uint8_t> data(size);
            pakstream.read(reinterpret_cast<char *>(data.data()), size);
            return data;
        }
    };

    struct wad_archive : archive_like
    {
        std::ifstream wadstream;

        // WAD Format
        struct wad_header
        {
            std::array<char, 4> identification { 'W', 'A', 'D', '2' };
            uint32_t numlumps;
            uint32_t infotableofs;

            auto stream_data()
            {
                return std::tie(identification, numlumps, infotableofs);
            }
        };

        struct wad_lump_header
        {
            uint32_t filepos;
            uint32_t disksize;
            uint32_t size; // uncompressed
            uint8_t type;
            uint8_t compression;
            uint8_t pad1, pad2;
            std::array<char, 16> name; // must be null terminated

            auto stream_data()
            {
                return std::tie(filepos, disksize, size, type, compression, pad1, pad2, name);
            }
        };

        std::unordered_map<std::string, std::tuple<uint32_t, uint32_t>, case_insensitive_hash, case_insensitive_equal> files;

        inline wad_archive(const path &pathname) :
            archive_like(pathname),
            wadstream(pathname, std::ios_base::in | std::ios_base::binary)
        {
            wadstream >> endianness<std::endian::little>;

            wad_header header;

            wadstream >= header;

            if (header.identification != std::array<char, 4> { 'W', 'A', 'D', '2' }) {
                throw std::exception("Bad magic");
            }

            files.reserve(header.numlumps);

            wadstream.seekg(header.infotableofs);

            for (size_t i = 0; i < header.infotableofs; i++) {
                wad_lump_header file;

                wadstream >= file;

                files[file.name.data()] = std::make_tuple(file.filepos, file.disksize);
            }
        }
        
        virtual data load(const path &filename) override
        {
            auto it = files.find(filename.generic_string());

            if (it == files.end()) {
                return std::nullopt;
            }

            wadstream.seekg(std::get<0>(it->second));
            uintmax_t size = std::get<1>(it->second);
            std::vector<uint8_t> data(size);
            wadstream.read(reinterpret_cast<char *>(data.data()), size);
            return data;
        }
    };
    
    std::list<std::shared_ptr<archive_like>> archives, directories;

    /** It's possible to compile quake 1/hexen 2 maps without a qdir */
    void clear()
    {
        qdir.clear();
        gamedir.clear();
        basedir.clear();

        archives.clear();
        directories.clear();
    }

    std::shared_ptr<archive_like> addArchive(const path &p)
    {
        if (!exists(p)) {
            FLogPrint("WARNING: '{}' not found\n", p);
            return nullptr;
        }

        if (is_directory(p)) {
            for (auto &dir : directories) {
                if (equivalent(dir->pathname, p)) {
                    return dir;
                }
            }

            auto &arch = directories.emplace_front(std::make_unique<directory_archive>(p));
            LogPrint(LOG_VERBOSE, "Added directory '{}'\n", p);
            return arch;
        } else {
            for (auto &arch : archives) {
                if (equivalent(arch->pathname, p)) {
                    return arch;
                }
            }

            auto ext = p.extension();

            try {
                if (string_iequals(ext.generic_string(), ".pak")) {
                    auto &arch = archives.emplace_front(std::make_shared<pak_archive>(p));
                    auto &pak = reinterpret_cast<std::shared_ptr<pak_archive> &>(arch);
                    LogPrint(LOG_VERBOSE, "Added pak '{}' with {} files\n", p, pak->files.size());
                    return arch;
                } else if (string_iequals(ext.generic_string(), ".wad")) {
                    auto &arch = archives.emplace_front(std::make_shared<wad_archive>(p));
                    auto &wad = reinterpret_cast<std::shared_ptr<wad_archive> &>(arch);
                    LogPrint(LOG_VERBOSE, "Added wad '{}' with {} lumps\n", p, wad->files.size());
                    return arch;
                } else {
                    FLogPrint("WARNING: no idea what to do with archive '{}'\n", p);
                }
            }
            catch (std::exception e) {
                FLogPrint("WARNING: unable to load archive '{}': {}\n", p, e.what());
            }
        }

        return nullptr;
    }

    data load(const path &p)
    {
        // check absolute + relative first
        if (exists(p)) {
            uintmax_t size = file_size(p);
            std::ifstream stream(p, std::ios_base::in | std::ios_base::binary);
            std::vector<uint8_t> data(size);
            stream.read(reinterpret_cast<char *>(data.data()), size);
            return data;
        }
        
        // check direct archive loading
        auto archive = splitArchivePath(p);

        if (std::get<0>(archive).has_relative_path()) {
            auto &arch = addArchive(std::get<0>(archive));

            if (arch) {
                return arch->load(std::get<1>(archive));
            }
        }

        // absolute doesn't make sense for other load types
        if (p.is_absolute()) {
            return std::nullopt;
        }

        // check directories
        for (auto &dir : directories) {
            if (auto file = dir->load(p)) {
                return file;
            }
        }

        // check archives
        for (auto &arch : archives) {
            if (auto file = arch->load(p)) {
                return file;
            }
        }

        return std::nullopt;
    }
}

qfile_t SafeOpenWrite(const std::filesystem::path &filename)
{
    FILE *f;

#ifdef _WIN32
    f = _wfopen(filename.c_str(), L"wb");
#else
    f = fopen(filename.string().c_str(), "wb");
#endif

    if (!f)
        FError("Error opening {}: {}", filename, strerror(errno));

    return {f, fclose};
}

qfile_t SafeOpenRead(const std::filesystem::path &filename, bool must_exist)
{
    FILE *f;

#ifdef _WIN32
    f = _wfopen(filename.c_str(), L"rb");
#else
    f = fopen(filename.string().c_str(), "rb");
#endif

    if (!f) {
        if (must_exist)
            FError("Error opening {}: {}", filename, strerror(errno));

        return {nullptr, nullptr};
    }

    return {f, fclose};
}

size_t SafeRead(const qfile_t &f, void *buffer, size_t count)
{
    if (fread(buffer, 1, count, f.get()) != (size_t)count)
        FError("File read failure");

    return count;
}

size_t SafeWrite(const qfile_t &f, const void *buffer, size_t count)
{
    if (fwrite(buffer, 1, count, f.get()) != (size_t)count)
        FError("File write failure");

    return count;
}

void SafeSeek(const qfile_t &f, long offset, int32_t origin)
{
    fseek(f.get(), offset, origin);
}

long SafeTell(const qfile_t &f)
{
    return ftell(f.get());
}

struct pakheader_t
{
    char magic[4];
    unsigned int tableofs;
    unsigned int numfiles;
};

struct pakfile_t
{
    char name[56];
    unsigned int offset;
    unsigned int length;
};

/*
 * ==============
 * LoadFilePak
 * reads a file directly out of a pak, to make re-lighting friendlier
 * writes to the filename, stripping the pak part of the name
 * ==============
 */
long LoadFilePak(std::filesystem::path &filename, void *destptr)
{
    // check if we have a .pak file in this path
    for (auto p = filename.parent_path(); !p.empty() && p != p.root_path(); p = p.parent_path()) {
        if (p.extension() == ".pak") {
            qfile_t file = SafeOpenRead(p);

            // false positive
            if (!file)
                continue;

            // got one; calculate the relative remaining path
            auto innerfile = filename.lexically_relative(p);

            uint8_t **bufferptr = static_cast<uint8_t **>(destptr);
            pakheader_t header;
            long length = -1;
            SafeRead(file, &header, sizeof(header));

            header.numfiles = LittleLong(header.numfiles) / sizeof(pakfile_t);
            header.tableofs = LittleLong(header.tableofs);

            if (!strncmp(header.magic, "PACK", 4)) {
                pakfile_t *files = new pakfile_t[header.numfiles];

                SafeSeek(file, header.tableofs, SEEK_SET);
                SafeRead(file, files, header.numfiles * sizeof(*files));

                for (uint32_t i = 0; i < header.numfiles; i++) {
                    if (innerfile == files[i].name) {
                        SafeSeek(file, files[i].offset, SEEK_SET);
                        *bufferptr = new uint8_t[files[i].length + 1];
                        SafeRead(file, *bufferptr, files[i].length);
                        length = files[i].length;
                        break;
                    }
                }
                delete[] files;
            }

            if (length < 0)
                FError("Unable to find '{}' inside '{}'", innerfile, filename);

            filename = innerfile;
            return length;
        }
    }

    // not in a pak, so load it normally
    return LoadFile(filename, destptr);
}

/*
 * ===========
 * filelength
 * ===========
 */
static long Sys_FileLength(const qfile_t &f)
{
    long pos = ftell(f.get());
    fseek(f.get(), 0, SEEK_END);
    long end = ftell(f.get());
    fseek(f.get(), pos, SEEK_SET);

    return end;
}
/*
 * ==============
 * LoadFile
 * ==============
 */
long LoadFile(const std::filesystem::path &filename, void *destptr)
{
    uint8_t **bufferptr = static_cast<uint8_t **>(destptr);

    qfile_t file = SafeOpenRead(filename, true);

    long length = Sys_FileLength(file);

    uint8_t *buffer = *bufferptr = new uint8_t[length + 1];

    if (!buffer)
        FError("allocation of {} bytes failed.", length);

    SafeRead(file, buffer, length);

    buffer[length] = 0;

    return length;
}