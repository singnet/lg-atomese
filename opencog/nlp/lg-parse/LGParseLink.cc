/*
 * LGParseLink.cc
 *
 * Copyright (C) 2017 Linas Vepstas
 *
 * Author: Linas Vepstas <linasvepstas@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <atomic>
#include <uuid/uuid.h>
#include <link-grammar/link-includes.h>

#include <opencog/atoms/atom_types/NameServer.h>
#include <opencog/atoms/base/Node.h>
#include <opencog/atoms/core/NumberNode.h>
#include <opencog/atomspace/AtomSpace.h>
#include <opencog/nlp/lg-dict/LGDictNode.h>
#include "LGParseLink.h"

using namespace opencog;
void error_handler(lg_errinfo *ei, void *data);

/// The expected format of an LgParseLink is:
///
///     LgParseLink
///         PhraseNode "this is a test."
///         LgDictNode "en"
///         NumberNode  6   -- optional, number of parses.
///
/// When executed, the result of parsing the phrase text, using the
/// specified dictionary, is placed in the atomspace.  Execution
/// returns a Sentencenode pointing at the parse results.  If the third,
/// optional NumberNode is present, then that will be the number of
/// parses that are captured. If the NumberNode is not present, it
/// defaults to four.
///
/// The LgParseLink is a kind of FunctionLink, and can thus be used in
/// any expression that FunctionLinks can be used with.
///
void LGParseLink::init()
{
	const HandleSeq& oset = _outgoing;

	size_t osz = oset.size();
	if (2 != osz and 3 != osz)
		throw InvalidParamException(TRACE_INFO,
			"LGParseLink: Expecting two or three arguments, got %lu", osz);

	Type pht = oset[0]->get_type();
	if (PHRASE_NODE != pht and VARIABLE_NODE != pht and GLOB_NODE != pht)
		throw InvalidParamException(TRACE_INFO,
			"LGParseLink: Expecting PhraseNode, got %s",
			oset[0]->to_string().c_str());

	Type dit = oset[1]->get_type();
	if (LG_DICT_NODE != dit and VARIABLE_NODE != dit and GLOB_NODE != dit)
		throw InvalidParamException(TRACE_INFO,
			"LGParseLink: Expecting LgDictNode, got %s",
			oset[1]->to_string().c_str());

	if (3 == osz)
	{
		Type nit = oset[2]->get_type();
		if (NUMBER_NODE != nit and VARIABLE_NODE != nit and GLOB_NODE != nit)
			throw InvalidParamException(TRACE_INFO,
				"LGParseLink: Expecting NumberNode, got %s",
				oset[2]->to_string().c_str());
	}
}

LGParseLink::LGParseLink(const HandleSeq&& oset, Type t)
	: FunctionLink(std::move(oset), t)
{
	// Type must be as expected
	if (not nameserver().isA(t, LG_PARSE_LINK))
	{
		const std::string& tname = nameserver().getTypeName(t);
		throw InvalidParamException(TRACE_INFO,
			"Expecting an LgParseLink, got %s", tname.c_str());
	}
	init();
}

LGParseMinimal::LGParseMinimal(const HandleSeq&& oset, Type t)
	: LGParseLink(std::move(oset), t)
{
	// Type must be as expected
	if (not nameserver().isA(t, LG_PARSE_MINIMAL))
	{
		const std::string& tname = nameserver().getTypeName(t);
		throw InvalidParamException(TRACE_INFO,
			"Expecting an LgParseMinimal, got %s", tname.c_str());
	}
	init();
}

// =================================================================

ValuePtr LGParseLink::execute(AtomSpace* as, bool silent)
{
	if (PHRASE_NODE != _outgoing[0]->get_type())
		throw InvalidParamException(TRACE_INFO,
			"LGParseLink: Invalid outgoing set at 0; expecting PhraseNode");
	if (LG_DICT_NODE != _outgoing[1]->get_type())
		throw InvalidParamException(TRACE_INFO,
			"LGParseLink: Invalid outgoing set at 1; expecting LgDictNode");
	if (3 == _outgoing.size() and
	   NUMBER_NODE != _outgoing[2]->get_type())
		throw InvalidParamException(TRACE_INFO,
			"LGParseLink: Invalid outgoing set at 2; expecting NumberNode");

	// Link grammar, for some reason, has a different error handler
	// per thread. Don't know why. So we have to set it every time,
	// because we don't know what thread we are in.
	lg_error_set_handler(error_handler, nullptr);

	// Get the dictionary
	LgDictNodePtr ldn(LgDictNodeCast(_outgoing[1]));
	Dictionary dict = ldn->get_dictionary();
	if (nullptr == dict)
		throw InvalidParamException(TRACE_INFO,
			"LgParseLink requires valid dictionary! \"%s\" was given.",
			ldn->get_name().c_str());

	// Set up the sentence
	const char* phrstr = _outgoing[0]->get_name().c_str() ;
	Sentence sent = sentence_create(phrstr, dict);
	if (nullptr == sent)
		throw FatalErrorException(TRACE_INFO,
			"LGParseLink: Unexpected parser failure!");

	// Work with the default parse options (mostly).
	// Suppress printing of combinatorial-overflow warning.
	// Set timeout to 60 seconds; the default is infinite.
	Parse_Options opts = parse_options_create();
	parse_options_set_verbosity(opts, 0);
	parse_options_set_max_parse_time(opts, 60);

	// The number of linkages to process.
	int max_linkages = 0;
	if (3 == _outgoing.size())
	{
		NumberNodePtr nnp(NumberNodeCast(_outgoing[2]));
		max_linkages = nnp->get_value() + 0.5;
		parse_options_set_linkage_limit(opts, max_linkages);
	}
	else
	{
		// Avoid ingesting an absurdly large number of
		// linkages. If the user really wants more, they
		// need to ask for more.
		parse_options_set_linkage_limit(opts, 100);
	}

	// XXX FIXME -- We should fish parse options out of the atomspace.
	// Something like this, maybe:
	//     EvaluationLink
	//         PredicateNode "LG ParseTime"
	//         ListLink
	//             LgDictNode "En_US"
	//             NumberNode 42
	// ... or something like that ...

	// For the ANY language, this code is being used for sampling.
	// In this case, we are not concerned about reproducibility,
	// but want different, truly random results each time through.
	// Viz, every time we have a four-word sentence, we want a
	// different parse for it, each time. Bug #3065.
	parse_options_set_repeatable_rand(opts, 0);

	// Count the number of parses.
	int num_linkages = sentence_parse(sent, opts);
	if (num_linkages < 0)
	{
		sentence_delete(sent);
		parse_options_delete(opts);
		throw FatalErrorException(TRACE_INFO,
			"LGParseLink: Unexpected parser error while parsing >>%s<<", phrstr);
	}

	// If num_links is zero, try again, allowing null linked words.
	if (num_linkages == 0)
	{
		parse_options_reset_resources(opts);
		parse_options_set_min_null_count(opts, 1);
		parse_options_set_max_null_count(opts, sentence_length(sent));
		num_linkages = sentence_parse(sent, opts);
	}

	if (num_linkages <= 0)
	{
		sentence_delete(sent);
		parse_options_delete(opts);
		throw RuntimeException(TRACE_INFO,
			"LGParseLink: Parser timeout.");
	}

	// Post-processor might not accept all of the parses.
	num_linkages = sentence_num_valid_linkages(sent);

	// Takes limit from parameter only if it's positive and smaller
	if ((max_linkages > 0) && (max_linkages < num_linkages))
	{
		num_linkages = max_linkages;
	}

	// Hmm. I hope that uuid_generate() won't block if there is not
	// enough entropy in the entropy pool....
	uuid_t uu;
	uuid_generate(uu);
	char idstr[37];
	uuid_unparse(uu, idstr);

	char sentstr[sizeof(idstr) + 10] = "sentence@";
	strcat(sentstr, idstr);

	Handle snode(as->add_node(SENTENCE_NODE, sentstr));

	// Avoid generating big piles of Atoms, if the user did not
	// want them. (The extra Atoms deescribe disjuncts, etc.)
	bool minimal = (get_type() == LG_PARSE_MINIMAL);

	// There are only so many parses available.
	int num_available = sentence_num_linkages_post_processed(sent);

	int jct = 0;
	for (int i=0; jct<num_linkages and i<num_available; i++)
	{
		// Skip sentences with P.P. violations.
		if (0 < sentence_num_violations(sent, i)) continue;
		jct ++;
		Linkage lkg = linkage_create(i, sent, opts);
		Handle pnode = cvt_linkage(lkg, i, sentstr, phrstr, minimal, as);
		as->add_link(PARSE_LINK, pnode, snode);
		linkage_delete(lkg);
	}

	sentence_delete(sent);
	parse_options_delete(opts);
	lg_error_flush();
	lg_error_clearall();
	return snode;
}

static std::atomic<unsigned long> wcnt;

Handle LGParseLink::cvt_linkage(Linkage lkg, int i, const char* idstr,
                                const char* phrstr,
                                bool minimal, AtomSpace* as) const
{
	char parseid[80];
	snprintf(parseid, 80, "%s_parse_%d", idstr, i);
	Handle pnode(as->add_node(PARSE_NODE, parseid));

	// Loop over all the words.
	HandleSeq wrds;
	int nwords = linkage_get_num_words(lkg);
	for (int w=0; w<nwords; w++)
	{
		size_t sb = linkage_get_word_byte_start(lkg, w);
		size_t eb = linkage_get_word_byte_end(lkg, w);

		// Problem: the default LG API supplies the word together with
		// the subscript, and with word regex and guess-marks. We really
		// do NOT want that crud. So use the byte offsets to get the
		// actual original string.  Since LEFT-WALL and RIGHT-WALL have
		// no offsets, we need to handle those differently.
		// strndupa allocates on stack, no need to free.
		const char* wrd;
		if (eb == sb) // Both are equal to -1
		{
			wrd = linkage_get_word(lkg, w);
		}
		else
		{
			// eb points at the byte after the last char,
			// its a great place to write a null byte.
			wrd = strndupa(phrstr + sb, eb-sb);
		}

		// LEFT-WALL is not an ordinary word. Its special. Make it
		// extra-special by adding "illegal" punctuation to it.
		// FYI, this is compatible with Relex, relex2logic.
		if (0 == w and 0 == strcmp(wrd, "LEFT-WALL")) wrd = "###LEFT-WALL###";
		if (nwords-1 == w and 0 == strcmp(wrd, "RIGHT-WALL")) wrd = "###RIGHT-WALL###";

		uuid_t uu2;
		uuid_generate(uu2);
		char idstr2[37];
		uuid_unparse(uu2, idstr2);
		char buff[801] = "";
		snprintf(buff, sizeof(buff), "%s@%s", wrd, idstr2);
		Handle winst(as->add_node(WORD_INSTANCE_NODE, buff));
		wrds.push_back(winst);

		// Associate the word with the parse.
		as->add_link(WORD_INSTANCE_LINK, winst, pnode);
		as->add_link(REFERENCE_LINK, winst,
			as->add_node(WORD_NODE, wrd));
		as->add_link(WORD_SEQUENCE_LINK, winst,
			Handle(createNumberNode(++wcnt)));

		// Don't bother with disjuncts for the minimal parses.
		if (minimal) continue;

		// Convert the disjunct to atomese.
		// This requires parsing a string. Fortunately, the
		// string is a very simple format.
		const char* djstr = linkage_get_disjunct_str(lkg, w);

		HandleSeq conseq;
		const char* p = djstr;
		while (*p)
		{
			while (' ' == *p) p++;
			if (0 == *p) break;
			bool multi = false;
			if ('@' == *p) { multi = true; p++; }
			const char* s = strchr(p, ' ');
			size_t len = s-p-1;
			if (NULL == s) len = strlen(p) - 1;
			char cstr[60];
			if (60 <= len)
				throw RuntimeException(TRACE_INFO,
					"LGParseLink: Dictionary has a bug; Uuexpectedly long connector=%s", djstr);
			strncpy(cstr, p, len);
			cstr[len] = 0;
			Handle con(createNode(LG_CONN_NODE, cstr));
			cstr[0] = *(p+len);
			cstr[1] = 0;
			Handle dir(createNode(LG_CONN_DIR_NODE, cstr));
			p = p+len+1;

			HandleSeq cono;
			cono.push_back(con);
			cono.push_back(dir);
			if (multi)
			{
				Handle mu(createNode(LG_CONN_MULTI_NODE, "@"));
				cono.push_back(mu);
			}
			Handle conl(createLink(std::move(cono), LG_CONNECTOR));
			conseq.push_back(conl);
		}

		// Set up the disjuncts on each word
		if (0 < conseq.size())
			as->add_link(LG_WORD_CSET, winst,
				as->add_link(LG_AND, std::move(conseq)));
	}

	// Loop over all the links
	int nlinks = linkage_get_num_links(lkg);
	for (int lk=0; lk<nlinks; lk++)
	{
		int lword = linkage_get_link_lword(lkg, lk);
		int rword = linkage_get_link_rword(lkg, lk);
		Handle lst(as->add_link(LIST_LINK, wrds[lword], wrds[rword]));

		// The link
		const char* label = linkage_get_link_label(lkg, lk);
		Handle lrel(as->add_node(LG_LINK_NODE, label));
		as->add_link(EVALUATION_LINK, lrel, lst);

		// Don't bother with the link instances for the minimal parse.
		if (minimal) continue;

		// The link instance.
		char buff[140];
		snprintf(buff, 140, "%s@%s-link-%d", label, parseid, lk);
		Handle linst(as->add_node(LG_LINK_INSTANCE_NODE, buff));
		as->add_link(EVALUATION_LINK, linst, lst);

		// The relation between link and link instance
		as->add_link(REFERENCE_LINK, linst, lrel);

		// The connectors for the link instance.
		const char* llab = linkage_get_link_llabel(lkg, lk);
		const char* rlab = linkage_get_link_rlabel(lkg, lk);
		as->add_link(LG_LINK_INSTANCE_LINK,
			linst,
			as->add_link(LG_CONNECTOR,
				as->add_node(LG_CONN_NODE, llab),
				as->add_node(LG_CONN_DIR_NODE, "+")),
			as->add_link(LG_CONNECTOR,
				as->add_node(LG_CONN_NODE, rlab),
				as->add_node(LG_CONN_DIR_NODE, "-")));
	}

	return pnode;
}

DEFINE_LINK_FACTORY(LGParseLink, LG_PARSE_LINK)

/* ===================== END OF FILE ===================== */
