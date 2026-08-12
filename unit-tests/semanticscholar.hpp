// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/semanticscholar.hpp
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
