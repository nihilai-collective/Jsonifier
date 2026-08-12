// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/round_trip/semanticscholar.hpp
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

template<document_or_value simdjson_type> inline void get_value(simdjson_type val_new, author& data_new) {
	simdjson::ondemand::object obj{ get_object(val_new) };
	get_field(obj, "name", data_new.name);
	get_field(obj, "ids", data_new.ids);
}

template<document_or_value simdjson_type> inline void get_value(simdjson_type val_new, semantic_scholar_element& data_new) {
	simdjson::ondemand::object obj{ get_object(val_new) };
	get_field(obj, "entities", data_new.entities);
	get_field(obj, "magId", data_new.magId);
	get_field(obj, "journalVolume", data_new.journalVolume);
	get_field(obj, "journalPages", data_new.journalPages);
	get_field(obj, "pmid", data_new.pmid);
	get_field(obj, "fieldsOfStudy", data_new.fieldsOfStudy);
	get_field(obj, "year", data_new.year);
	get_field(obj, "outCitations", data_new.outCitations);
	get_field(obj, "s2Url", data_new.s2Url);
	get_field(obj, "s2PdfUrl", data_new.s2PdfUrl);
	get_field(obj, "id", data_new.id);
	get_field(obj, "authors", data_new.authors);
	get_field(obj, "journalName", data_new.journalName);
	get_field(obj, "paperAbstract", data_new.paperAbstract);
	get_field(obj, "inCitations", data_new.inCitations);
	get_field(obj, "pdfUrls", data_new.pdfUrls);
	get_field(obj, "title", data_new.title);
	get_field(obj, "doi", data_new.doi);
	get_field(obj, "sources", data_new.sources);
	get_field(obj, "doiUrl", data_new.doiUrl);
	get_field(obj, "venue", data_new.venue);
}

template<> struct glz::meta<author> {
	using value_type			= author;
	static constexpr auto value = object(&value_type::name, &value_type::ids);
};

template<> struct glz::meta<semantic_scholar_element> {
	using value_type = semantic_scholar_element;
	static constexpr auto value =
		object(&value_type::entities, &value_type::magId, &value_type::journalVolume, &value_type::journalPages, &value_type::pmid, &value_type::fieldsOfStudy, &value_type::year,
			&value_type::outCitations, &value_type::s2Url, &value_type::s2PdfUrl, &value_type::id, &value_type::authors, &value_type::journalName, &value_type::paperAbstract,
			&value_type::inCitations, &value_type::pdfUrls, &value_type::title, &value_type::doi, &value_type::sources, &value_type::doiUrl, &value_type::venue);
};

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
