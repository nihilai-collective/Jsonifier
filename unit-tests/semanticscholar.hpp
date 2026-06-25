/*
	MIT License

	Copyright (c) 2024 RealTimeChris

	Permission is hereby granted, free of charge, to any person obtaining a copy of this
	software and associated documentation files (the "Software"), to deal in the Software
	without restriction, including without limitation the rights to use, copy, modify, merge,
	publish, distribute, sublicense, and/or sell copies of the Software, and to permit
	persons to whom the Software is furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all copies or
	substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
	INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
	PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
	FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
	OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
	DEALINGS IN THE SOFTWARE.
*/
/// https://github.com/RealTimeChris/Json-Performance
#pragma once

#include "common.hpp"

struct author {
	std::string name;
	std::vector<std::string> ids;
};

struct semantic_scholar_element {
	std::vector<std::string> entities;
	std::string magId;
	std::string journalVolume;
	std::string journalPages;
	std::string pmid;
	std::vector<std::string> fieldsOfStudy;
	std::optional<int64_t> year;
	std::vector<std::string> outCitations;
	std::string s2Url;
	std::string s2PdfUrl;
	std::string id;
	std::vector<author> authors;
	std::string journalName;
	std::string paperAbstract;
	std::vector<std::string> inCitations;
	std::vector<std::string> pdfUrls;
	std::string title;
	std::string doi;
	std::vector<std::string> sources;
	std::string doiUrl;
	std::string venue;
};

using semantic_scholar_message = std::vector<semantic_scholar_element>;

template<> struct jsonifier::core<author> {
	using value_type				 = author;
	static constexpr auto parseValue = createValue<&value_type::name, &value_type::ids>();
};

template<> struct jsonifier::core<semantic_scholar_element> {
	using value_type				 = semantic_scholar_element;
	static constexpr auto parseValue = createValue<&value_type::entities, &value_type::magId, &value_type::journalVolume, &value_type::journalPages, &value_type::pmid,
		&value_type::fieldsOfStudy, &value_type::year, &value_type::outCitations, &value_type::s2Url, &value_type::s2PdfUrl, &value_type::id, &value_type::authors,
		&value_type::journalName, &value_type::paperAbstract, &value_type::inCitations, &value_type::pdfUrls, &value_type::title, &value_type::doi, &value_type::sources,
		&value_type::doiUrl, &value_type::venue>();
};
