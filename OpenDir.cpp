#include <cstdlib>
#include <iostream>
#include <dirent.h>

using namespace std;

int main()
{
    DIR* directory;
    directory = opendir("/home/hhabib/Documents/Courses/");

    if (!directory)
    {
        cout << "Could not open directory" << endl;
        return 1;
    }

    struct dirent* dr;

    while (dr = readdir(directory))
    {
        cout << dr -> d_name << endl;
    }

    closedir(directory);

    return EXIT_SUCCESS;
}
