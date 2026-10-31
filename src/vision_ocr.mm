//===--------------------------------------------------------------------===//
// vision_ocr.mm — Apple Vision implementation (macOS only, compiled with ARC).
// See vision_ocr.hpp for the contract. No duckdb, poppler or tesseract
// headers here.
//
// Nothing throws inside an @autoreleasepool or while a CGImage is held:
// RecognizeLines reports failure as a message and the public functions throw
// after releasing everything.
//===--------------------------------------------------------------------===//
#include "vision_ocr.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Vision/Vision.h>

#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace pdf_ocr {
namespace {

std::string FromNSString(NSString *s) {
	const char *utf8 = s ? [s UTF8String] : nullptr;
	return utf8 ? std::string(utf8) : std::string();
}

// Tesseract language codes -> Vision BCP-47 identifiers.
const std::map<std::string, std::string> &TesseractToVision() {
	static const std::map<std::string, std::string> table = {
	    {"eng", "en-US"}, {"fra", "fr-FR"}, {"ita", "it-IT"}, {"deu", "de-DE"}, {"spa", "es-ES"},
	    {"por", "pt-BR"}, {"chi_sim", "zh-Hans"}, {"chi_tra", "zh-Hant"}, {"jpn", "ja-JP"}, {"kor", "ko-KR"},
	    {"rus", "ru-RU"}, {"ukr", "uk-UA"}, {"tha", "th-TH"}, {"vie", "vi-VT"}, {"ara", "ar-SA"},
	    {"tur", "tr-TR"}, {"ind", "id-ID"}, {"ces", "cs-CZ"}, {"dan", "da-DK"}, {"nld", "nl-NL"},
	    {"nor", "no-NO"}, {"nob", "nb-NO"}, {"nno", "nn-NO"}, {"msa", "ms-MY"}, {"pol", "pl-PL"},
	    {"ron", "ro-RO"}, {"swe", "sv-SE"},
	};
	return table;
}

struct Token {
	std::string text;
	CGRect box; // normalized, origin bottom-left
};

struct Line {
	std::string text;
	double confidence = 0; // 0..1
	std::vector<Token> tokens;
};

NSArray<NSString *> *SupportedLanguages(VNRecognizeTextRequest *request) API_AVAILABLE(macos(10.15)) {
	NSError *error = nil;
	NSArray<NSString *> *supported = nil;
	if (@available(macOS 12.0, *)) {
		supported = [request supportedRecognitionLanguagesAndReturnError:&error];
	} else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		supported = [VNRecognizeTextRequest supportedRecognitionLanguagesForTextRecognitionLevel:request.recognitionLevel
		                                                                                revision:request.revision
		                                                                                   error:&error];
#pragma clang diagnostic pop
	}
	return supported ? supported : @[];
}

// "eng+fra" -> @[@"en-US", @"fr-FR"]; BCP-47 names pass through. Every entry
// must be one Vision supports. '~name' (Tesseract's exclusion) is skipped.
// Returns nil and sets `error` when a language is unsupported.
NSArray<NSString *> *VisionLanguages(VNRecognizeTextRequest *request, const std::string &language, std::string &error)
    API_AVAILABLE(macos(10.15)) {
	NSArray<NSString *> *supported = SupportedLanguages(request);
	NSMutableArray<NSString *> *out = [NSMutableArray array];
	const std::string spec = language.empty() ? "eng" : language;
	size_t start = 0;
	while (start <= spec.size()) {
		size_t end = spec.find('+', start);
		if (end == std::string::npos) {
			end = spec.size();
		}
		const std::string name = spec.substr(start, end - start);
		start = end + 1;
		if (name.empty() || name[0] == '~') {
			continue;
		}
		auto mapped = TesseractToVision().find(name);
		const std::string wanted = mapped == TesseractToVision().end() ? name : mapped->second;
		NSString *candidate = [NSString stringWithUTF8String:wanted.c_str()];
		NSString *match = nil;
		for (NSString *s in supported) {
			if ([s caseInsensitiveCompare:candidate] == NSOrderedSame) {
				match = s;
				break;
			}
		}
		if (!match) {
			error = "language '" + name + "' is not supported by Vision on this system; supported: " +
			        FromNSString([supported componentsJoinedByString:@", "]);
			return nil;
		}
		[out addObject:match];
	}
	if (out.count == 0) {
		error = "no language selected in '" + language + "'";
		return nil;
	}
	return out;
}

// One accurate, language-corrected recognition pass. Returns an error
// message, or an empty string on success.
std::string RecognizeLines(CGImageRef image, CGImagePropertyOrientation orientation, const std::string &language,
                           bool want_tokens, std::vector<Line> &lines) {
	if (@available(macOS 10.15, *)) {
		std::string error;
		@autoreleasepool {
			VNRecognizeTextRequest *request = [[VNRecognizeTextRequest alloc] init];
			request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
			request.usesLanguageCorrection = YES;
			NSArray<NSString *> *languages = VisionLanguages(request, language, error);
			if (!languages) {
				return error;
			}
			request.recognitionLanguages = languages;
			VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCGImage:image
			                                                                    orientation:orientation
			                                                                        options:@{}];
			NSError *failure = nil;
			if (![handler performRequests:@[ request ] error:&failure]) {
				return "recognition failed: " + FromNSString(failure.localizedDescription);
			}
			NSCharacterSet *space = [NSCharacterSet whitespaceAndNewlineCharacterSet];
			for (VNRecognizedTextObservation *observation in request.results) {
				VNRecognizedText *top = [[observation topCandidates:1] firstObject];
				if (!top) {
					continue;
				}
				Line line;
				line.text = FromNSString(top.string);
				line.confidence = top.confidence;
				if (want_tokens) {
					NSString *s = top.string;
					const NSUInteger length = s.length;
					NSUInteger i = 0;
					while (i < length) {
						while (i < length && [space characterIsMember:[s characterAtIndex:i]]) {
							i++;
						}
						const NSUInteger begin = i;
						while (i < length && ![space characterIsMember:[s characterAtIndex:i]]) {
							i++;
						}
						if (i == begin) {
							continue;
						}
						const NSRange range = NSMakeRange(begin, i - begin);
						VNRectangleObservation *box = [top boundingBoxForRange:range error:nil];
						line.tokens.push_back(
						    {FromNSString([s substringWithRange:range]), box ? box.boundingBox : observation.boundingBox});
					}
				}
				lines.push_back(std::move(line));
			}
		}
		return error;
	}
	return "requires macOS 10.15 or later";
}

CGImageRef CreateImage(const unsigned char *data, int width, int height, int bytes_per_row, ImageFormat format) {
	const bool rgb24 = format == ImageFormat::RGB24;
	CGDataProviderRef provider = CGDataProviderCreateWithData(
	    nullptr, data, static_cast<size_t>(bytes_per_row) * static_cast<size_t>(height), nullptr);
	if (!provider) {
		return nullptr;
	}
	CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	// poppler ARGB32 is B,G,R,A in little-endian memory; rendered pages are opaque.
	const CGBitmapInfo info = rgb24 ? static_cast<CGBitmapInfo>(kCGImageAlphaNone)
	                                : static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
	CGImageRef image = CGImageCreate(static_cast<size_t>(width), static_cast<size_t>(height), 8, rgb24 ? 24 : 32,
	                                 static_cast<size_t>(bytes_per_row), space, info, provider, nullptr, false,
	                                 kCGRenderingIntentDefault);
	CGColorSpaceRelease(space);
	CGDataProviderRelease(provider);
	return image;
}

// Decodes with ImageIO and reads the EXIF orientation so photos come out upright.
CGImageRef DecodeImage(const unsigned char *data, size_t size, CGImagePropertyOrientation &orientation) {
	orientation = kCGImagePropertyOrientationUp;
	CFDataRef bytes = CFDataCreateWithBytesNoCopy(nullptr, data, static_cast<CFIndex>(size), kCFAllocatorNull);
	CGImageSourceRef source = bytes ? CGImageSourceCreateWithData(bytes, nullptr) : nullptr;
	CGImageRef image = source ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
	if (image) {
		CFDictionaryRef properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr);
		if (properties) {
			auto value = static_cast<CFNumberRef>(CFDictionaryGetValue(properties, kCGImagePropertyOrientation));
			uint32_t raw = 0;
			if (value && CFNumberGetValue(value, kCFNumberSInt32Type, &raw) && raw >= 1 && raw <= 8) {
				orientation = static_cast<CGImagePropertyOrientation>(raw);
			}
			CFRelease(properties);
		}
	}
	if (source) {
		CFRelease(source);
	}
	if (bytes) {
		CFRelease(bytes);
	}
	return image;
}

int MeanConfidence(const std::vector<Line> &lines) {
	if (lines.empty()) {
		return 0;
	}
	double sum = 0;
	for (auto &line : lines) {
		sum += line.confidence;
	}
	return static_cast<int>(std::lround(100.0 * sum / lines.size()));
}

void FillText(const std::vector<Line> &lines, TextResult &out) {
	for (auto &line : lines) {
		out.text += line.text;
		out.text.push_back('\n');
	}
	out.confidence = MeanConfidence(lines);
}

// Throws, unless best_effort asks for an empty result instead.
bool Failed(const std::string &error, const Options &opt) {
	if (error.empty()) {
		return false;
	}
	if (opt.best_effort) {
		return true;
	}
	throw std::runtime_error("ocr backend 'vision': " + error);
}

} // namespace

bool VisionAvailable() {
	if (@available(macOS 10.15, *)) {
		return true;
	}
	return false;
}

TextResult RecognizeVisionText(const unsigned char *data, int width, int height, int bytes_per_row,
                               ImageFormat format, const Options &opt) {
	TextResult out;
	out.backend_used = Backend::Vision;
	if (!data || width <= 0 || height <= 0) {
		return out;
	}
	std::vector<Line> lines;
	CGImageRef image = CreateImage(data, width, height, bytes_per_row, format);
	std::string error = image ? RecognizeLines(image, kCGImagePropertyOrientationUp, opt.language, false, lines)
	                          : "could not wrap the page bitmap as an image";
	if (image) {
		CGImageRelease(image);
	}
	if (!Failed(error, opt)) {
		FillText(lines, out);
	}
	return out;
}

WordsResult RecognizeVisionWords(const unsigned char *data, int width, int height, int bytes_per_row,
                                 ImageFormat format, const Options &opt) {
	WordsResult out;
	out.backend_used = Backend::Vision;
	if (!data || width <= 0 || height <= 0) {
		return out;
	}
	std::vector<Line> lines;
	CGImageRef image = CreateImage(data, width, height, bytes_per_row, format);
	std::string error = image ? RecognizeLines(image, kCGImagePropertyOrientationUp, opt.language, true, lines)
	                          : "could not wrap the page bitmap as an image";
	if (image) {
		CGImageRelease(image);
	}
	if (Failed(error, opt)) {
		return out;
	}
	// Vision's normalized boxes share PDF user space's bottom-left origin.
	const double dpi = opt.dpi > 0 ? opt.dpi : 300;
	const double page_w = width * 72.0 / dpi;
	const double page_h = height * 72.0 / dpi;
	for (auto &line : lines) {
		for (auto &token : line.tokens) {
			if (!HasGlyphs(token.text)) {
				continue;
			}
			Word w;
			w.text = token.text;
			w.x0 = CGRectGetMinX(token.box) * page_w;
			w.x1 = CGRectGetMaxX(token.box) * page_w;
			w.y0 = CGRectGetMinY(token.box) * page_h;
			w.y1 = CGRectGetMaxY(token.box) * page_h;
			w.confidence = static_cast<float>(100.0 * line.confidence);
			out.words.push_back(std::move(w));
		}
	}
	out.confidence = MeanConfidence(lines);
	return out;
}

TextResult RecognizeVisionImageBlob(const unsigned char *data, size_t size, const Options &opt) {
	TextResult out;
	out.backend_used = Backend::Vision;
	if (opt.format != "text") {
		throw std::runtime_error("ocr backend 'vision': format must be 'text' (hocr and tsv are Tesseract formats)");
	}
	CGImagePropertyOrientation orientation;
	CGImageRef image = DecodeImage(data, size, orientation);
	std::vector<Line> lines;
	std::string error = image ? RecognizeLines(image, orientation, opt.language, false, lines)
	                          : "could not decode image BLOB (expected a format ImageIO reads, e.g. PNG or JPEG)";
	if (image) {
		CGImageRelease(image);
	}
	if (!Failed(error, opt)) {
		FillText(lines, out);
	}
	return out;
}

} // namespace pdf_ocr
