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
#include <vector>

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
            std::string name = a.substr(2), value;
            auto eq = name.find('=');
            if (eq != std::string::npos)
            {
                value = name.substr(eq + 1);
                name = name.substr(0, eq);
            }
            else if (flags.count(name))
                value = "1";
            else if (i + 1 < argc)
                value = argv[++i];
            else
            {
                bad_ = "--" + name + " needs a value";
                return;
            }
            values_[name] = value;  // the last one wins
            every_[name].push_back(value);
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
    // Every value an option was given, in order (--param a=1 --param b=2).
    std::vector<std::string> all(const std::string& n) const
    {
        auto it = every_.find(n);
        return it == every_.end() ? std::vector<std::string>{} : it->second;
    }

private:
    std::map<std::string, std::string> values_;
    std::map<std::string, std::vector<std::string>> every_;
    std::string bad_;
};

} // namespace fennectv

#endif
