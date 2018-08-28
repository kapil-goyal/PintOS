#include <fstream>
#include <iostream>
#include <string>
#include <stdio.h>
#include <time.h>

// Get current date/time, format is YYYY-MM-DD.HH:mm:ss
const std::string currentDateTime() {
    time_t     now = time(0);
    struct tm  tstruct;
    char       buf[80];
    tstruct = *localtime(&now);
    strftime(buf, sizeof(buf), "%Y-%m-%d.%X", &tstruct);
    return buf;
}

int main(){
	std::ofstream logfile;
	logfile.open("log_file.txt",std::ios_base::app);
	logfile << "Current date and time to update log: " << currentDateTime() << std::endl;
	std:: string s;
	std:: cout << "Members present: " <<std::endl;
	std::getline (std::cin,s);
	logfile << "Members present: " << s << std::endl;

	std:: cout << "Short Description of work: " <<std::endl;
	std::getline (std::cin,s);
	logfile << "Short Description of work: " << s << std::endl;

	std:: cout << "Duration of session: " <<std::endl;
	std::getline (std::cin,s);
	logfile << "Duration of session: " << s << std::endl;

	std:: cout << "Name of files affected: " <<std::endl;
	std::getline (std::cin,s);
	logfile << "Name of files affected: " << s << std::endl;

	std:: cout << "Estimated lines of code affected: " <<std::endl;
	std::getline (std::cin,s);
	logfile << "Estimated lines of code affected: " << s << std::endl << std::endl;
	
	logfile.close();
	return 0;
}