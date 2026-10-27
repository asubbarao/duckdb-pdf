#pragma once

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace pdf_office {
struct Application {
	std::string name;
	std::string bundle_id;
	std::string open;
	std::string export_pdf;
};

inline std::vector<Application> Applications(std::string extension) {
	std::transform(extension.begin(), extension.end(), extension.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	const Application excel {"Microsoft Excel", "com.microsoft.Excel",
	                         "set d to open workbook workbook file name (POSIX path of sourceFile)",
	                         "save workbook as d filename outputPath file format PDF file format"};
	const Application word {"Microsoft Word", "com.microsoft.Word", "open sourceFile\nset d to document sourceName",
	                        "save as d file name outputPath file format format PDF"};
	const Application powerpoint {"Microsoft PowerPoint", "com.microsoft.Powerpoint",
	                              "open sourceFile\nset d to presentation sourceName",
	                              "save d in outputFile as save as PDF"};
	const Application numbers {"Numbers", "com.apple.iWork.Numbers", "set d to open sourceFile",
	                           "export d to outputFile as PDF"};
	const Application pages {"Pages", "com.apple.iWork.Pages", "set d to open sourceFile",
	                         "export d to outputFile as PDF"};
	const Application keynote {"Keynote", "com.apple.iWork.Keynote", "set d to open sourceFile",
	                           "export d to outputFile as PDF"};
	// Native iWork files cannot be opened by Microsoft applications.
	if (extension == ".numbers")
		return {numbers};
	if (extension == ".pages")
		return {pages};
	if (extension == ".key")
		return {keynote};
	if (extension == ".xlsx" || extension == ".xls" || extension == ".xlsm" || extension == ".xltx" ||
	    extension == ".xltm" || extension == ".csv" || extension == ".tsv")
		return {excel, numbers};
	if (extension == ".docx" || extension == ".doc" || extension == ".docm" || extension == ".dotx" ||
	    extension == ".dotm" || extension == ".rtf")
		return {word, pages};
	if (extension == ".pptx" || extension == ".ppt" || extension == ".pptm" || extension == ".ppsx" ||
	    extension == ".pps" || extension == ".potx")
		return {powerpoint, keynote};
	return {};
}

inline std::string Script(const Application &app) {
	// Only fixed application metadata enters source; all filesystem paths are argv.
	return "on run argv\n"
	       "set sourceFile to POSIX file (item 1 of argv)\n"
	       "set sourceName to name of (info for sourceFile)\n"
	       "set outputPath to item 2 of argv\n"
	       "set outputFile to POSIX file outputPath\n"
	       "with timeout of 110 seconds\n"
	       "tell application id \"" +
	       app.bundle_id +
	       "\"\n"
	       "set d to missing value\ntry\n" +
	       app.open + "\n" + app.export_pdf +
	       "\nclose d saving no\non error messageText number errorNumber\n"
	       "if d is not missing value then\ntry\nclose d saving no\nend try\nend if\n"
	       "error messageText number errorNumber\nend try\nend tell\nend timeout\nend run";
}

// Native callback returns false only for an unavailable app; conversion failures
// throw and proceed to the next backend. The LibreOffice callback is always last.
template <class Native, class LibreOffice>
void Convert(const std::vector<Application> &apps, Native native, LibreOffice libreoffice) {
	std::string failures;
	for (const auto &app : apps) {
		try {
			if (native(app))
				return;
			failures += app.name + ": not installed; ";
		} catch (const std::exception &error) {
			failures += app.name + ": " + error.what() + "; ";
		}
	}
	try {
		libreoffice();
		return;
	} catch (const std::exception &error) {
		throw std::runtime_error(failures + "LibreOffice: " + error.what());
	}
}
} // namespace pdf_office
