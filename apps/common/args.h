// A small --name value parser for the command-line tools.
#ifndef FENNECTV_APPS_ARGS_H
#define FENNECTV_APPS_ARGS_H

#include <cstdio>
#include <cstdlib>
#include <locale>
#include <map>
#include <sstream>
#include <set>
#include <string>

namespace fennectv
{

class Args
{
public:
    // flags: options that take no value.
    Args(int argc, char** argv, const std::set<std::string>& flags)
    {
        for (int i = 1; i < argc; i++)
        {
            std::string a = argv[i];
            if (a.rfind("--", 0) != 0)
            {
                bad_ = "unexpected argument: " + a;
                return;
            }
            std::string name = a.substr(2);
            auto eq = name.find('=');
            if (eq != std::string::npos)
                values_[name.substr(0, eq)] = name.substr(eq + 1);
            else if (flags.count(name))
                values_[name] = "1";
            else if (i + 1 < argc)
                values_[name] = argv[++i];
            else
                bad_ = "--" + name + " needs a value";
        }
    }

    const std::string& bad() const { return bad_; }
    bool has(const std::string& n) const { return values_.count(n) > 0; }
    std::string str(const std::string& n, const std::string& def = "") const
    {
        auto it = values_.find(n);
        return it == values_.end() ? def : it->second;
    }
    double num(const std::string& n, double def) const
    {
        auto it = values_.find(n);
        if (it == values_.end()) return def;
        // Not atof: Qt sets the user's locale, and in Polish "0.15" reads
        // as 0. A point or a comma, whatever the locale.
        std::string v = it->second;
        for (char& c : v)
            if (c == ',') c = '.';
        std::istringstream in(v);
        in.imbue(std::locale::classic());
        double d = def;
        in >> d;
        return d;
    }
    int integer(const std::string& n, int def) const { return (int)num(n, def); }

private:
    std::map<std::string, std::string> values_;
    std::string bad_;
};

} // namespace fennectv

#endif
