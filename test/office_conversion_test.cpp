#include "pdf_office_conversion.hpp"
#include <iostream>

static void Require(bool pass, const char *message) {
	if (!pass)
		throw std::runtime_error(message);
}

static void TestOfficeConversion() {
	using namespace pdf_office;
	Require(Applications(".XLSX").at(0).name == "Microsoft Excel", "Excel priority");
	Require(Applications(".docx").at(0).name == "Microsoft Word", "Word priority");
	Require(Applications(".pptx").at(0).name == "Microsoft PowerPoint", "PowerPoint priority");
	for (const auto &ext : {".numbers", ".pages", ".key"}) {
		auto apps = Applications(ext);
		Require(apps.size() == 1 && apps[0].name.find("Microsoft") == std::string::npos, "native iWork routing");
	}
	Require(Applications(".html").empty(), "markup uses LibreOffice");
	for (const auto &ext : {".xlsx", ".docx", ".pptx"}) {
		auto apps = Applications(ext);
		std::vector<std::string> calls;
		Convert(
		    apps,
		    [&](const Application &a) {
			    calls.push_back(a.name);
			    return true;
		    },
		    [&] { calls.push_back("LibreOffice"); });
		Require(calls.size() == 1 && calls[0] == apps[0].name, "successful primary stops fallback");
		calls.clear();
		Convert(
		    apps,
		    [&](const Application &a) {
			    calls.push_back(a.name);
			    return a.name == apps[1].name;
		    },
		    [&] { calls.push_back("LibreOffice"); });
		Require(calls.size() == 2 && calls[1] == apps[1].name, "missing Microsoft uses Apple");
		calls.clear();
		Convert(
		    apps,
		    [&](const Application &a) -> bool {
			    calls.push_back(a.name);
			    throw std::runtime_error("export rejected");
		    },
		    [&] { calls.push_back("LibreOffice"); });
		Require(calls.size() == 3 && calls[2] == "LibreOffice", "native failures use LibreOffice");
		try {
			Convert(
			    apps, [](const Application &) -> bool { throw std::runtime_error("denied"); },
			    [] { throw std::runtime_error("converter absent"); });
			throw std::logic_error("missing combined failure");
		} catch (const std::runtime_error &error) {
			std::string message = error.what();
			Require(message.find(apps[0].name) != std::string::npos &&
			            message.find(apps[1].name) != std::string::npos &&
			            message.find("LibreOffice: converter absent") != std::string::npos,
			        "all failures retained");
		}
		for (const auto &app : apps) {
			auto script = Script(app);
			Require(script.find("item 1 of argv") != std::string::npos &&
			            script.find("item 2 of argv") != std::string::npos,
			        "paths are literal argv");
			Require(script.find("quit") == std::string::npos, "never quit user's application");
			Require(script.find("on error") != std::string::npos, "cleanup on export error");
		}
	}
	bool libreoffice_ran = false;
	Convert(
	    Applications(".html"), [](const Application &) { return false; }, [&] { libreoffice_ran = true; });
	Require(libreoffice_ran, "LibreOffice handles other formats");
	std::cout << "Office routing, fallback, error retention and script construction passed\n";
}

int main() {
	try {
		TestOfficeConversion();
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
