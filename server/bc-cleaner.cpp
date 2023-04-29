#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include <dirent.h>
#include <pwd.h>
#include <grp.h>

#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <bsd/string.h>

#include "bc-cleaner.h"
#include "bc-server.h"
#include "bc-syslog.h"

bool bc_is_leap_year(int year)
{
	return (year % 4 == 0 &&
		(year % 100 != 0 ||
		year % 400 == 0));
}

int bc_days_in_month(int year, int month)
{
	static const int days_per_month[] =
		{
			31, 28, 31, 30,
			31, 30, 31, 31,
			30, 31, 30, 31
		};

	if (--month < 0 || (size_t)month > sizeof(days_per_month) - 1)
	{
		bc_log(Error, "Time check error, invalid month: %d", month);
		return 0;
	}

	int days = days_per_month[month];
	if (month == 2 && bc_is_leap_year(year)) days = 29;

	return days;
}

bool bc_compare_time(const bc_time& tm1, const bc_time& tm2)
{
	long long time1 = 0, time2 = 0;
	int days1 = 0, days2 = 0;

	// Convert years and months to days
	for (int y = 1; y < tm1.year; y++)
		days1 += bc_is_leap_year(y) ? 366 : 365;

	for (int m = 1; m < tm1.month; m++)
		days1 += bc_days_in_month(tm1.year, m);

	for (int y = 1; y < tm2.year; y++)
		days2 += bc_is_leap_year(y) ? 366 : 365;

	for (int m = 1; m < tm2.month; m++)
		days2 += bc_days_in_month(tm2.year, m);

	// Add days
	days1 += tm1.day;
	days2 += tm2.day;

	// Convert days to seconds
	time1 = (long long)days1 * 24 * 60 * 60;
	time2 = (long long)days2 * 24 * 60 * 60;

	// Convert hours to seconds
	time1 += (long long)tm1.hour * 60 * 60;
	time2 += (long long)tm2.hour * 60 * 60;

	// Convert minutes to seconds
	time1 += (long long)tm1.min * 60;
	time2 += (long long)tm2.min * 60;

	// Add seconds
	time1 += tm1.sec;
	time2 += tm2.sec;

	return time1 < time2;
}

bool bc_is_base_path(std::string dir_path)
{
	if (!dir_path.length()) return false;
	char basepath[PATH_MAX];

	if (bc_get_media_loc(basepath, sizeof(basepath)) < 0)
	{
		bc_log(Error, "No free storage locations for new recordings!");
		return false;
	}

	std::string base_path = std::string(basepath);
	if (base_path.back() != '/') base_path.append("/");
	if (dir_path.back() != '/') dir_path.append("/");

	if (dir_path.length() <= base_path.length() ||
		dir_path == base_path) return true;

	return false;
}

bool bc_is_current_path(const std::string& path)
{
	std::string timed_path = path.substr(path.size() - 30, 26);
	bc_time rec_time;
	int id = 0;

	int count = sscanf(timed_path.c_str(), "%04d/%02d/%02d/%06d/%02d-%02d-%02d",
		(int*)&rec_time.year, (int*)&rec_time.month, (int*)&rec_time.day, (int*)&id,
		(int*)&rec_time.hour, (int*)&rec_time.min, (int*)&rec_time.sec);
	if (count != 7) return true;

	struct tm tm;
	time_t ts = time(NULL);
	localtime_r(&ts, &tm);

	/* Do not remove current recording directory */
	if (rec_time.year == (tm.tm_year + 1900) &&
		rec_time.month == (tm.tm_mon + 1) &&
		rec_time.day == tm.tm_mday) return true;

	return false;
}

std::string bc_get_parent_path(const std::string& path)
{
	if (!path.length()) return std::string();
	std::string dir_path = path;

	std::size_t last_slash = dir_path.find_last_of('/');
	if (last_slash == std::string::npos) return std::string();

	dir_path.erase(last_slash);
	return dir_path;
}

bool bc_is_dir_empty(const std::string& path)
{
	int file_count = 0;
	struct dirent *entry;

	bc_log(Debug, "Cheking if empty: %s", path.c_str());
	DIR *dir = opendir(path.c_str());

	if (dir == NULL) {
		bc_log(Warning, "Can not open directory %s: %s",
			path.c_str(), strerror(errno));

		return false;
	}

	// Check if there is something more than "." and ".."
	while ((entry = readdir(dir)) != NULL)
		if (++file_count > 2) break;

	closedir(dir);

	return file_count <= 2 ?
		true : false;
}

std::string bc_get_dir_path(const std::string& path)
{
	/* Do not empty or remove base paths */
	if (bc_is_base_path(path)) return std::string();
	else if (!path.length()) return std::string();

	struct stat path_stat;
	if (stat(path.c_str(), &path_stat) != 0)
	{
		if (errno == ENOENT)
		{
			/* If file does not exist, try parent directory */
			std::string parent = bc_get_parent_path(path);
			return bc_get_dir_path(parent);
		}

		bc_log(Error, "Failed to stat: %s (%s)",
			path.c_str(), strerror(errno));

		return std::string();
	}

	if (S_ISDIR(path_stat.st_mode))
		return std::string(path);

	if (S_ISREG(path_stat.st_mode))
	{
		std::string parent = bc_get_parent_path(path);
		return bc_get_dir_path(parent);
	}

	bc_log(Error, "The path is neither a file nor a directory: %s", path.c_str());
	return std::string();
}

bool bc_remove_dir_if_empty(const std::string& path)
{
	// Do not remove current working dir, even if its empty
	if (bc_is_current_path(path)) return false;

	// Get parent directory path of recording file
	std::string dir = bc_get_dir_path(path);
	if (!dir.length()) return false;

	while (dir.length())
	{
		if (bc_is_base_path(dir)) break;
		if (!bc_is_dir_empty(dir)) break;

		if (rmdir(dir.c_str()) != 0)
		{
			bc_log(Error, "Failed to delete directory: %s (%s)",
				dir.c_str(), strerror(errno));

			return false;
		}

		bc_log(Info, "Deleted empty directory: %s", dir.c_str());
		std::string parent = bc_get_parent_path(dir);
		dir = bc_get_dir_path(parent);
	}

	return true;
}