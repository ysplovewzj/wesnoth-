/*
	Copyright (C) 2005 - 2023
	by Guillaume Melquiond <guillaume.melquiond@gmail.com>
	Copyright (C) 2003 by David White <dave@whitevine.net>
	Part of the Battle for Wesnoth Project https://www.wesnoth.org/

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY.

	See the COPYING file for more details.
*/

#define GETTEXT_DOMAIN "wesnoth-lib"

#include "formula/string_utils.hpp"
#include "variable.hpp"

#include "config.hpp"
#include "log.hpp"
#include "gettext.hpp"

static lg::log_domain log_engine("engine");
#define ERR_NG LOG_STREAM(err, log_engine)
#define WRN_NG LOG_STREAM(warn, log_engine)

static bool two_dots(char a, char b) { return a == '.' && b == '.'; }

namespace utils {

	namespace detail {
		std::string(* evaluate_formula)(const std::string& formula) = nullptr;
	}

template <typename T>
class string_map_variable_set : public variable_set
{
public:
	string_map_variable_set(const std::map<std::string,T>& map) : map_(map) {}

	virtual config::attribute_value get_variable_const(const std::string &key) const
	{
		config::attribute_value val;
		const auto itor = map_.find(key);
		if (itor != map_.end())
			val = itor->second;
		return val;
	}
	virtual variable_access_const get_variable_access_read(const std::string& varname) const
	{
		temp_.reset(new config);
		for(const auto& p : map_) {
			temp_->insert(p.first, p.second);
		}
		return variable_access_const(varname, *temp_);
	}
private:
	const std::map<std::string,T>& map_;
	mutable std::shared_ptr<config> temp_; // only used if get_variable_access_read called
};
}

static std::string do_interpolation(const std::string &str, const variable_set& set)
{
	std::string res = str;
	// This needs to be able to store negative numbers to check for the while's condition
	// (which is only false when the previous '$' was at index 0)
	int rfind_dollars_sign_from = res.size();
	while(rfind_dollars_sign_from >= 0) {
		// Going in a backwards order allows nested variable-retrieval, e.g. in arrays.
		// For example, "I am $creatures[$i].user_description!"
		const std::string::size_type var_begin_loc = res.rfind('$', rfind_dollars_sign_from);

		// If there are no '$' left then we're done.
		if(var_begin_loc == std::string::npos) {
			break;
		}

		// For the next iteration of the loop, search for more '$'
		// (not from the same place because sometimes the '$' is not replaced)
		rfind_dollars_sign_from = static_cast<int>(var_begin_loc) - 1;


		const std::string::iterator var_begin = res.begin() + var_begin_loc;

		// The '$' is not part of the variable name.
		const std::string::iterator var_name_begin = var_begin + 1;
		std::string::iterator var_end = var_name_begin;

		if(var_name_begin == res.end()) {
			// Any '$' at the end of a string is just a '$'
			continue;
		} else if(*var_name_begin == '(') {
			// The $( ... ) syntax invokes a formula
			int paren_nesting_level = 0;
			bool in_string = false,
				in_comment = false;
			do {
				switch(*var_end) {
				case '(':
					if(!in_string && !in_comment) {
						++paren_nesting_level;
					}
					break;
				case ')':
					if(!in_string && !in_comment) {
						--paren_nesting_level;
					}
					break;
				case '#':
					if(!in_string) {
						in_comment = !in_comment;
					}
					break;
				case '\'':
					if(!in_comment) {
						in_string = !in_string;
					}
					break;
				// TODO: support escape sequences when/if they are allowed in FormulaAI strings
				}
			} while(++var_end != res.end() && paren_nesting_level > 0);
			if(utils::detail::evaluate_formula == nullptr) {
				WRN_NG << "Formula substitution ignored (and removed) because WFL engine is not present in the server.";
				res.replace(var_begin, var_end, "");
				continue;
			}
			if(paren_nesting_level > 0) {
				ERR_NG << "Formula in WML string cannot be evaluated due to "
					<< "a missing closing parenthesis:\n\t--> \""
					<< std::string(var_begin, var_end) << "\"";
				res.replace(var_begin, var_end, "");
				continue;
			}
			res.replace(var_begin, var_end, utils::detail::evaluate_formula(std::string(var_begin+2, var_end-1)));
			continue;
		}

		// Find the maximum extent of the variable name (it may be shortened later).
		for(int bracket_nesting_level = 0; var_end != res.end(); ++var_end) {
			const char c = *var_end;
			if(c == '[') {
				++bracket_nesting_level;
			}
			else if(c == ']') {
				if(--bracket_nesting_level < 0) {
					break;
				}
			}
			// isascii() breaks on mingw with -std=c++0x
			else if (!(((c) & ~0x7f) == 0)/*isascii(c)*/ || (!isalnum(c) && c != '.' && c != '_')) {
				break;
			}
		}

		// Two dots in a row cannot be part of a valid variable name.
		// That matters for random=, e.g. $x..$y
		var_end = std::adjacent_find(var_name_begin, var_end, two_dots);
		// the default value is specified after ''?'
		const std::string::iterator default_start = var_end < res.end() && *var_end == '?' ? var_end + 1 : res.end();

		// If the last character is '.', then it can't be a sub-variable.
		// It's probably meant to be a period instead. Don't include it.
		// Would need to do it repetitively if there are multiple '.'s at the end,
		// but don't actually need to do so because the previous check for adjacent '.'s would catch that.
		// For example, "My score is $score." or "My score is $score..."
		if(*(var_end-1) == '.'
		// However, "$array[$i]" by itself does not name a variable,
		// so if "$array[$i]." is encountered, then best to include the '.',
		// so that it more closely follows the syntax of a variable (if only to get rid of all of it).
		// (If it's the script writer's error, they'll have to fix it in either case.)
		// For example in "$array[$i].$field_name", if field_name does not exist as a variable,
		// then the result of the expansion should be "", not "." (which it would be if this exception did not exist).
		&& *(var_end-2) != ']') {
			--var_end;
		}

		const std::string var_name(var_name_begin, var_end);
		if(default_start == res.end()) {
			if(var_end != res.end() && *var_end == '|') {
				// It's been used to end this variable name; now it has no more effect.
				// This can allow use of things like "$$composite_var_name|.x"
				// (Yes, that's a WML 'pointer' of sorts. They are sometimes useful.)
				// If there should still be a '|' there afterwards to affect other variable names (unlikely),
				// just put another '|' there, one matching each '$', e.g. "$$var_containing_var_name||blah"
				++var_end;
			}


			if (var_name.empty()) {
				// Allow for a way to have $s in a string.
				// $| will be replaced by $.
				res.replace(var_begin, var_end, "$");
			}
			else {
				// The variable is replaced with its value.
				res.replace(var_begin, var_end,
					set.get_variable_const(var_name));
			}
		}
		else {
			var_end = default_start;
			while(var_end != res.end() && *var_end != '|') {
				++var_end;
			}
			const std::string::iterator default_end = var_end;
			const config::attribute_value& val = set.get_variable_const(var_name);
			if(var_end == res.end()) {
				res.replace(var_begin, default_start - 1, val);
			}
			else if(!val.empty()) {
				res.replace(var_begin, var_end + 1, val);
			}
			else {
				res.replace(var_begin, var_end + 1, std::string(default_start, default_end));
			}
		}
	}

	return res;
}

namespace utils {

std::string interpolate_variables_into_string(const std::string &str, const string_map * const symbols)
{
	auto set = string_map_variable_set<t_string>(*symbols);
	return do_interpolation(str, set);
}

std::string interpolate_variables_into_string(const std::string &str, const std::map<std::string,std::string> * const symbols)
{
	auto set = string_map_variable_set<std::string>(*symbols);
	return do_interpolation(str, set);
}

std::string interpolate_variables_into_string(const std::string &str, const variable_set& variables)
{
	return do_interpolation(str, variables);
}

t_string interpolate_variables_into_tstring(const t_string &tstr, const variable_set& variables)
{
	if(!tstr.str().empty()) {
		std::string interp = utils::interpolate_variables_into_string(tstr.str(), variables);
		if(tstr.str() != interp) {
			return t_string(interp);
		}
	}
	return tstr;
}

std::string format_conjunct_list(const t_string& empty, const std::vector<t_string>& elems) {
	switch(elems.size()) {
	case 0: return empty;
	case 1: return elems[0];
		// TRANSLATORS: Formats a two-element conjunctive list.
	case 2: return VGETTEXT("conjunct pair^$first and $second", {{"first", elems[0]}, {"second", elems[1]}});
	}
	// TRANSLATORS: Formats the first two elements of a conjunctive list.
	std::string prefix = VGETTEXT("conjunct start^$first, $second", {{"first", elems[0]}, {"second", elems[1]}});
	// For size=3 this loop is not entered
	for(std::size_t i = 2; i < elems.size() - 1; i++) {
		// TRANSLATORS: Formats successive elements of a conjunctive list.
		prefix = VGETTEXT("conjunct mid^$prefix, $next", {{"prefix", prefix}, {"next", elems[i]}});
	}
	// TRANSLATORS: Formats the final element of a conjunctive list.
	return VGETTEXT("conjunct end^$prefix, and $last", {{"prefix", prefix}, {"last", elems.back()}});
}

std::string format_disjunct_list(const t_string& empty, const std::vector<t_string>& elems) {
	switch(elems.size()) {
	case 0: return empty;
	case 1: return elems[0];
		// TRANSLATORS: Formats a two-element disjunctive list.
	case 2: return VGETTEXT("disjunct pair^$first or $second", {{"first", elems[0]}, {"second", elems[1]}});
	}
	// TRANSLATORS: Formats the first two elements of a disjunctive list.
	std::string prefix = VGETTEXT("disjunct start^$first, $second", {{"first", elems[0]}, {"second", elems[1]}});
	// For size=3 this loop is not entered
	for(std::size_t i = 2; i < elems.size() - 1; i++) {
		// TRANSLATORS: Formats successive elements of a disjunctive list.
		prefix = VGETTEXT("disjunct mid^$prefix, $next", {{"prefix", prefix}, {"next", elems[i]}});
	}
	// TRANSLATORS: Formats the final element of a disjunctive list.
	return VGETTEXT("disjunct end^$prefix, or $last", {{"prefix", prefix}, {"last", elems.back()}});
}

std::string format_timespan(std::time_t time, bool detailed)
{
	if(time <= 0) {
		return _("timespan^expired");
	}

	typedef std::tuple<std::time_t, const char*, const char*> time_factor;

	static const std::vector<time_factor> TIME_FACTORS{
		// TRANSLATORS: The "timespan^$num xxxxx" strings originating from the same file
		// as the string with this comment MUST be translated following the usual rules
		// for WML variable interpolation -- that is, without including or translating
		// the caret^ prefix, and leaving the $num variable specification intact, since
		// it is technically code. The only translatable natural word to be found here
		// is the time unit (year, month, etc.) For example, for French you would
		// translate "timespan^$num years" as "$num ans", thus allowing the game UI to
		// generate output such as "39 ans" after variable interpolation.
		time_factor{ 31104000, N_n("timespan^$num year",   "timespan^$num years")   }, // 12 months
		time_factor{ 2592000,  N_n("timespan^$num month",  "timespan^$num months")  }, // 30 days
		time_factor{ 604800,   N_n("timespan^$num week",   "timespan^$num weeks")   },
		time_factor{ 86400,    N_n("timespan^$num day",    "timespan^$num days")    },
		time_factor{ 3600,     N_n("timespan^$num hour",   "timespan^$num hours")   },
		time_factor{ 60,       N_n("timespan^$num minute", "timespan^$num minutes") },
		time_factor{ 1,        N_n("timespan^$num second", "timespan^$num seconds") },
	};

	std::vector<t_string> display_text;
	string_map i18n;

	for(const auto& factor : TIME_FACTORS) {
		const auto [ secs, fmt_singular, fmt_plural ] = factor;
		const int amount = time / secs;

		if(amount) {
			time -= secs * amount;
			i18n["num"] = std::to_string(amount);
			display_text.emplace_back(VNGETTEXT(fmt_singular, fmt_plural, amount, i18n));
			if(!detailed) {
				break;
			}
		}
	}

	return format_conjunct_list(_("timespan^expired"), display_text);
}

}

std::string vgettext_impl(const char *domain
		, const char *msgid
		, const utils::string_map& symbols)
{
	const std::string orig(translation::dsgettext(domain, msgid));
	const std::string msg = utils::interpolate_variables_into_string(orig, &symbols);
	return msg;
}

std::string vngettext_impl(const char* domain,
		const char* singular,
		const char* plural,
		int count,
		const utils::string_map& symbols)
{
	const std::string orig(translation::dsngettext(domain, singular, plural, count));
	const std::string msg = utils::interpolate_variables_into_string(orig, &symbols);
	return msg;
}

int edit_distance_approx(const std::string &str_1, const std::string &str_2)
{
	int edit_distance = 0;
	if(str_1.length() == 0) {
		return str_2.length();
	}
	else if(str_2.length() == 0) {
		return str_1.length();
	}
	else {
		int j = 0;
		int len_max = std::max(str_1.length(), str_2.length());
		for(int i = 0; i < len_max; i++) {
			if(str_1[i] != str_2[j]) {
				//SWAP
				if(str_1[i+1] == str_2[j] && str_1[i] == str_2[j+1]) {
					// No need to test the next letter
					i++;j++;
				}
				//ADDITION
				else if(str_1[i+1] == str_2[j]) {
					j--;
				}
				//DELETION
				else if(str_1[i] == str_2[j+1]) {
					i--;
				}
				// CHANGE (no need to do anything, next letter MAY be successful).
				edit_distance++;
				if(edit_distance * 100 / std::min(str_1.length(), str_2.length()) > 33) {
					break;
				}
			}
			j++;
		}
	}
	return edit_distance;
}
