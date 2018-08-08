#include <iostream>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>
#include <unordered_map>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/range.hpp>
#include <boost/functional/hash.hpp>
#include <primitives/filesystem.h>
#include <primitives/sw/main.h>
#include <primitives/sw/settings.h>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

struct Library;

struct LibraryLess
{
    bool operator()(const Library *l1, const Library *l2) const;
};

using Libraries = std::set<Library*, LibraryLess>;
using LibraryMap = std::map<String, Library*>;

const path out_dir = "out";
const path bs_insertions_file = "inserts.yml";
cl::opt<path> boost_dir("d", cl::desc("directory with boost sources"), cl::value_desc("boost dir"), cl::init("d:/dev/boost"));
cl::opt<String> version("v", cl::desc("used to open version.commits file"), cl::value_desc("boost version"), cl::Required);
cl::opt<bool> gen_sw("sw", cl::desc("generate sw script"));
String remote;
const String source = "tag";
String source_name = "boost-";
std::map<String, String> commits;

const std::map<String, std::set<String>> additional_src_deps = {
    {"thread", {"date_time"}},
};

LibraryMap libraries;

struct Library
{
    String name;
    Libraries deps;
    Libraries header_only_deps;
    Libraries src_deps;
    std::set<path> files;
    bool has_source_dir = false;

    bool contains_simple(Library* lib) const
    {
        if (deps.find(lib) != deps.end())
            return true;
        return false;
    }

    bool contains(Library* lib, std::set<const Library*> &checked) const
    {
        if (checked.find(this) != checked.end())
            return false;
        if (deps.find(lib) != deps.end())
            return true;
        checked.insert(this);
        for (auto &dep : deps)
        {
            if (dep->contains(lib, checked))
                return true;
        }
        checked.erase(this);
        return false;
    }

    bool requires_building() const
    {
        return has_source_dir;
    }

    String get_url() const
    {
        String name = this->name;
        if (name == "numeric/conversion")
            name = "numeric_conversion";
        if (name == "numeric/ublas")
            name = "ublas";
        if (name == "numeric/odeint")
            name = "odeint";
        if (name == "numeric/interval")
            name = "interval";
        String s = "https://github.com/boostorg/" + name;
        return s;
    }

    String get_name() const
    {
        if (name == "numeric/conversion")
            return "numeric";
        if (name == "numeric/ublas")
            return "ublas";
        if (name == "numeric/odeint")
            return "odeint";
        if (name == "numeric/interval")
            return "interval";
        if (name.find("numeric") == 0)
            return name.substr(strlen("numeric/"));
        return name;
    }

    String get_dir() const
    {
        return name;
    }

    std::set<Library*> get_all_deps() const
    {
        std::set<Library*> all_deps{(Library*)this};
        get_all_deps(all_deps);
        all_deps.erase((Library*)this);
        return all_deps;
    }

private:
    void get_all_deps(std::set<Library*> &all_deps) const
    {
        for (auto dep : deps)
        {
            if (all_deps.find(dep) != all_deps.end())
                continue;
            all_deps.insert(dep);
            dep->get_all_deps(all_deps);
        }
    }
};

bool LibraryLess::operator()(const Library *l1, const Library *l2) const
{
    return l1->get_name() < l2->get_name();
}

Library* get_library(String name)
{
    boost::algorithm::to_lower(name);

    Library *lib = libraries[name];
    if (!lib)
        lib = libraries[name] = new Library;
    return lib;
}

Library* has_library(String name)
{
    boost::algorithm::to_lower(name);

    auto i = libraries.find(name);
    if (i == libraries.end())
        return nullptr;
    return i->second;
}

String print_dot()
{
    String s;
    s += "digraph G {\n";
    for (auto &lib : libraries)
    {
        for (auto &dep : lib.second->deps)
            s += "    \"" + lib.first + "\" -> \"" + dep->name + "\";\n";
    }
    s += "}\n";
    return s;
}

void print_dot(const path &fn)
{
    auto s = print_dot();
    std::ofstream ofile(fn.string());
    ofile << s;
}

void read_json(const path &file)
{
    std::ifstream ifile(file.string());
    auto data = nlohmann::json::parse(ifile);

    for (auto it = data.begin(); it != data.end(); ++it)
    {
        auto lib = get_library(it.key());
        lib->name = it.key();
        for (auto &sdep : it.value())
        {
            String s = sdep;
            lib->deps.insert(get_library(s));
        }
        libraries[lib->name] = lib;
    }
}

void read_dir(const path &dir)
{
    // find files
    auto find_files = [](const path &dir, const String &prefix = String())
    {
        for (auto &f : boost::make_iterator_range(fs::directory_iterator(dir / prefix), {}))
        {
            if (!fs::is_directory(f))
                continue;

            auto p = f.path();
            auto lib = fs::relative(p, dir).string();
            boost::algorithm::replace_all(lib, "\\", "/");
            auto l = get_library(lib);
            l->name = lib;
            std::cout << "listing library dir: " << lib << "\n";

            for (auto &d : { "include", "src" })
            {
                if (!fs::exists(p / d))
                    continue;
                if (d == "src" && fs::exists(p / "build"))
                    l->has_source_dir = true;
                for (auto &f : boost::make_iterator_range(fs::recursive_directory_iterator(p / d), {}))
                {
                    if (!fs::is_regular_file(f))
                        continue;
                    l->files.insert(f);
                }
            }
        }
    };
    find_files(dir);
    find_files(dir, "numeric");
    libraries.erase("numeric"); // remove empty

    // read files
    std::regex r_include("#\\s*?include[^<\"]*?[<\"](boost/.*?)[>\"]");
    std::unordered_map<path, Library*> filemap;
    for (auto &lp : libraries)
    {
        auto &lib = lp.second;
        for (auto &p : lib->files)
        {
            auto rp = fs::relative(p, dir / lib->name / "include");
            if (filemap.find(rp) != filemap.end())
                std::cerr << "duplicate file: " << rp.string() << "\n";
            filemap[rp] = lib;
        }
    }
    for (auto &lp : libraries)
    {
        auto &lib = lp.second;
        std::cout << "processing library dir: " << lib->name << "\n";

        // read src deps
        if (lib->requires_building())
        {
            auto f = dir / lib->get_dir() / "build";
            String s;
            if (fs::exists(f / "Jamfile.v2"))
                s = read_file(f / "Jamfile.v2");
            else if (fs::exists(f / "Jamfile"))
                s = read_file(f / "Jamfile");
            else
            {
                std::cerr << "no jamfile found\n";
                continue;
            }
            std::regex r_lib1("<library>\\s*?/boost/([^/]*?)//boost_\\w+");
            std::regex r_lib2("/boost//(\\w+)");
            std::regex r_lib3("/build//boost_(\\w+)");
            std::regex r_lib4("<library>.*?//boost_(\\w+)");
            std::smatch m;
            while (
                std::regex_search(s, m, r_lib1) ||
                std::regex_search(s, m, r_lib2) ||
                std::regex_search(s, m, r_lib3) ||
                std::regex_search(s, m, r_lib4))
            {
                lib->src_deps.insert(get_library(m[1].str()));
                s = m.suffix();
            }
        }

        for (auto &f : lib->files)
        {
            auto s = read_file(f);
            std::smatch m;
            while (std::regex_search(s, m, r_include))
            {
                auto include = m[1].str();
                s = m.suffix();

                auto i = filemap.find(include);
                if (i != filemap.end())
                    lib->deps.insert(i->second);
                else
                    std::cerr << "cannot add a file: " << include << "\n";
            }
        }
    }

    // remove self
    for (auto &lib : libraries)
        lib.second->deps.erase(lib.second);
}

void write_libraries(const path &fn)
{
    nlohmann::json out;
    for (auto &lib : libraries)
    {
        std::set<String> names;
        for (auto &dep : lib.second->deps)
            names.insert(dep->name);
        nlohmann::json deps;
        for (auto &n : names)
            deps.push_back(n);
        out[lib.first] = deps;
    }
    std::ofstream ofile1(fn.string());
    ofile1.width(4);
    ofile1 << out;
}

void prepare()
{
    // gather all deps
    for (auto &lp : libraries)
    {
        auto &lib = lp.second;

        auto i = additional_src_deps.find(lib->get_name());
        if (i != additional_src_deps.end())
        {
            for (auto dep : i->second)
                lib->src_deps.insert(get_library(dep));
        }

        auto all_deps = lib->get_all_deps();
        lib->deps.insert(all_deps.begin(), all_deps.end());
    }

    // process
    for (auto &lp : libraries)
    {
        auto &lib = lp.second;

        lib->header_only_deps = lib->deps;
        lib->deps = lib->src_deps;
        for (auto dep : lib->deps)
            lib->header_only_deps.erase(dep);
    }
}

void process_simple()
{
    std::set<std::pair<Library*, Library*>> fails;
    bool next = true;
    for (auto it_lib = libraries.begin(); it_lib != libraries.end(); )
    {
        auto lib = it_lib->second;

        if (next)
        {
            std::cout << "processing (simple) " << lib->name << "\n";
            next = false;
        }

        bool stop = false;
        for (auto &dep : lib->deps)
        {
            for (auto &dep2 : lib->deps)
            {
                if (dep == dep2 || dep->deps.empty() || fails.find({ dep,dep2 }) != fails.end())
                    continue;
                if (dep->contains_simple(dep2))
                {
                    lib->deps.erase(dep2);
                    stop = true;
                    break;
                }
                else
                    fails.insert({ dep,dep2 });
            }
            if (stop)
                break;
        }
        if (!stop)
        {
            ++it_lib;
            next = true;
        }
    }
}

void process()
{
    std::set<std::pair<Library*, Library*>> fails;
    bool next = true;
    for (auto it_lib = libraries.begin(); it_lib != libraries.end(); )
    {
        auto lib = it_lib->second;

        if (next)
        {
            std::cout << "processing " << lib->name << "\n";
            next = false;
        }

        bool stop = false;
        for (auto &dep : lib->deps)
        {
            for (auto &dep2 : lib->deps)
            {
                if (dep == dep2 || dep->deps.empty() || fails.find({ dep,dep2 }) != fails.end())
                    continue;
                std::set<const Library*> checked;
                checked.insert(lib);
                if (dep->contains(dep2, checked))
                {
                    lib->deps.erase(dep2);
                    stop = true;
                    break;
                }
                else
                    fails.insert({ dep,dep2 });
            }
            if (stop)
                break;
        }
        if (!stop)
        {
            ++it_lib;
            next = true;
        }
    }
}

void post_process()
{
}

void write_yaml_cppan(const path &fn)
{
    String root_path = "pvt.cppan.demo.boost";

    YAML::Node inserts;
    inserts = YAML::LoadFile(bs_insertions_file.string());

    YAML::Node root;
    // make cppan happy
    // TODO: remove
    // FIXME: do not throw error when all sources are provided
    root["source"]["git"] = "https://github.com/boostorg";
    root["version"] = version.getValue();
    root["root_project"] = root_path;

    auto projects = root["projects"];
    for (auto i : inserts)
        projects[root_path + "." + i.first.as<String>()] = i.second;

    for (auto &lp : libraries)
    {
        auto &lib = lp.second;

        if (commits[lib->get_dir()].empty())
        {
            std::cerr << "no commit for lib: " << lib->get_name() << "\n";
            //continue;
        }

        YAML::Node project = projects[root_path + "." + lib->get_name()];
        project["type"] = "library";
        project["source"]["git"] = lib->get_url();
        project["source"]["commit"] = commits[lib->get_dir()];
        //project["source"][source] = source_name;
        //project["source"]["remote"] = remote;

        //if (!project["root_directory"].IsDefined())
        //    project["root_directory"] = "libs/" + lib->get_name();

        if (lib->get_name() == "log_setup")
            project["source"] = projects[root_path + ".log"]["source"];

        if (!project["files"].IsDefined())
        {
            project["files"].push_back("include/.*");
            if (lib->requires_building())
                project["files"].push_back("src/.*");
        }

        project["include_directories"]["public"].push_back("include");
        if (lib->requires_building())
            project["include_directories"]["private"].push_back("src");

        YAML::Node deps;
        for (auto &dep : lib->deps)
        {
            if (!lib->requires_building() && dep->requires_building())
            {
                YAML::Node header_dep;
                header_dep["name"] = root_path + "." + dep->get_name();
                header_dep["include_directories_only"] = true;
                deps.push_back(header_dep);
                continue;
            }
            deps.push_back(root_path + "." + dep->get_name());
        }
        for (auto &dep : lib->header_only_deps)
        {
            YAML::Node header_dep;
            header_dep["name"] = root_path + "." + dep->get_name();
            header_dep["include_directories_only"] = true;
            deps.push_back(header_dep);
        }
        if (!lib->deps.empty() || !lib->header_only_deps.empty())
        {
            for (auto d : deps)
                project["dependencies"].push_back(d);
        }

        if (lib->requires_building())
        {
            auto n = lib->get_name();
            boost::algorithm::to_upper(n);
            project["options"]["static"]["definitions"]["public"].push_back("BOOST_" + n + "_STATIC_LINK");
            project["options"]["static"]["definitions"]["public"].push_back("BOOST_ALL_STATIC_LINK");
            project["options"]["static"]["definitions"]["public"].push_back("BOOST_" + n + "_BUILD_LIB");

            project["options"]["shared"]["definitions"]["public"].push_back("BOOST_" + n + "_DYN_LINK");
            project["options"]["shared"]["definitions"]["public"].push_back("BOOST_ALL_DYN_LINK");
            project["options"]["shared"]["definitions"]["private"].push_back("BOOST_" + n + "_BUILD_DLL");
            project["options"]["shared"]["definitions"]["public"].push_back("BOOST_" + n + "_USE_DLL");

            project["options"]["any"]["definitions"]["private"].push_back("BOOST_" + n + "_SOURCE");
            project["options"]["any"]["definitions"]["private"].push_back("BOOST_" + n + "_BUILDING_THE_LIB");
        }

        if (!lib->requires_building())
            project["header_only"] = true;

        {
            error_code ec;
            auto p = fn.parent_path();
            fs::create_directories(p / "single", ec);
            fs::create_directories(p / "root", ec);

            {
                auto r = YAML::Clone(root);
                r["projects"] = YAML::Node();
                r["projects"][root_path + "." + lib->get_name()] = project;

                std::ofstream ofile((p / "root" / (lib->get_name() + ".yml")).string());
                ofile.width(4);
                ofile << r;
            }

            {
                auto r = YAML::Clone(project);
                r["version"] = root["version"];

                std::ofstream ofile((p / "single" / (lib->get_name() + ".yml")).string());
                ofile.width(4);
                ofile << r;
            }
        }
    }

    std::ofstream ofile1(fn.string());
    ofile1.width(4);
    ofile1 << root;
}

void write_yaml_sw(const path &fn)
{
    String root_path = "pvt.cppan.demo.boost";

    YAML::Node inserts;
    inserts = YAML::LoadFile(bs_insertions_file.string());

    YAML::Node root;
    // make cppan happy
    // TODO: remove
    // FIXME: do not throw error when all sources are provided
    root["source"]["git"] = "https://github.com/boostorg";
    root["version"] = version.getValue();
    root["root_project"] = root_path;

    auto projects = root["projects"];
    for (auto i : inserts)
        projects[root_path + "." + i.first.as<String>()] = i.second;

    String s_cpp_libs_ho, s_cpp_libs_compiled;
    String s_cpp_deps;

    for (auto &lp : libraries)
    {
        auto &lib = lp.second;

        if (commits[lib->get_dir()].empty())
        {
            std::cerr << "no commit for lib: " << lib->get_name() << "\n";
            //continue;
        }

        if (lib->requires_building())
            s_cpp_libs_compiled += "\"" + lib->get_name() + "\",";
        else
            s_cpp_libs_ho += "\"" + lib->get_name() + "\",";

        YAML::Node project = projects[root_path + "." + lib->get_name()];
        project["source"]["git"] = lib->get_url();
        project["source"]["commit"] = commits[lib->get_dir()];
        //project["source"][source] = source_name;
        //project["source"]["remote"] = remote;

        //if (!project["root_directory"].IsDefined())
        //    project["root_directory"] = "libs/" + lib->get_name();

        if (lib->get_name() == "log_setup")
            project["source"] = projects[root_path + ".log"]["source"];

        if (!project["files"].IsDefined())
        {
            project["files"].push_back("include/.*");
            if (lib->requires_building())
                project["files"].push_back("src/.*");
        }

        project["include_directories"]["public"].push_back("include");
        if (lib->requires_building())
            project["include_directories"]["private"].push_back("src");

        YAML::Node deps;
        for (auto &dep : lib->deps)
        {
            if (!lib->requires_building() && dep->requires_building())
            {
                YAML::Node header_dep;
                header_dep["name"] = root_path + "." + dep->get_name();
                header_dep["include_directories_only"] = true;
                deps.push_back(header_dep);

                s_cpp_deps += "add_public_dependency(\"" + lib->get_name() + "\", \"" + dep->get_name() + "\", true);\n";
                continue;
            }
            deps.push_back(root_path + "." + dep->get_name());

            s_cpp_deps += "add_public_dependency(\"" + lib->get_name() + "\", \"" + dep->get_name() + "\", false);\n";
        }
        for (auto &dep : lib->header_only_deps)
        {
            YAML::Node header_dep;
            header_dep["name"] = root_path + "." + dep->get_name();
            header_dep["include_directories_only"] = true;
            deps.push_back(header_dep);

            s_cpp_deps += "add_public_dependency(\"" + lib->get_name() + "\", \"" + dep->get_name() + "\", true);\n";
        }
    }

    {
        std::ofstream ofile1((fn.parent_path() / "cpp_libs_header_only.txt").string());
        ofile1 << s_cpp_libs_ho;
    }
    {
        std::ofstream ofile1((fn.parent_path() / "cpp_libs_compiled.txt").string());
        ofile1 << s_cpp_libs_compiled;
    }
    {
        std::ofstream ofile1((fn.parent_path() / "cpp_deps.txt").string());
        ofile1 << s_cpp_deps;
    }
}

void write_yaml(const path &fn)
{
    if (gen_sw)
        write_yaml_sw(fn);
    else
        write_yaml_cppan(fn);
}

void debug()
{
    read_json(out_dir / "processed.json");
    write_libraries(out_dir / "processed.json");
    print_dot(out_dir / "processed.dot");
    write_yaml(out_dir / "cppan.yml");
}

void release()
{
    //read_json("initial.json");
    read_dir(boost_dir / "libs");
    write_libraries(out_dir / "initial.json");
    print_dot(out_dir / "initial.dot");

    prepare();
    //process_simple();
    //process();
    post_process();

    write_libraries(out_dir / "processed.json");
    print_dot(out_dir / "processed.dot");
    write_yaml(out_dir / "cppan.yml");
}

int main(int argc, char* argv[])
{
    cl::ParseCommandLineOptions(argc, argv);

    source_name += version;

    std::istringstream ss(read_file(version + ".commits"));
    while (1)
    {
        String lib, commit;
        ss >> lib >> commit;
        if (!ss)
            break;
        lib = lib.substr(5); // length of "libs/"
        commits[lib] = commit;
    }

    /*if (argc > 3)
        remote = argv[3];
    else
    {
        std::cerr << "No remote specified\n";
        std::cerr << "usage: boost_deps boost_dir version remote\n";
        return 1;
    }*/

    fs::create_directories(out_dir);
    release();
    return 0;
}
