%module(directors="1") xapian

%{
/* xapian.i: the Xapian scripting interface.
 *
 * Copyright 1999,2000,2001 BrightStation PLC
 * Copyright 2001,2002 Ananova Ltd
 * Copyright 2002,2003,2005 James Aylett
 * Copyright 2002,2003,2004,2005,2006,2007,2008,2009,2010 Olly Betts
 * Copyright 2007 Lemur Consulting Ltd
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301
 * USA
 */
%}

// In C#, we wrap ++ and -- as ++ and --.
#ifdef SWIGCSHARP
#define NEXT(RET, CLASS) CLASS & next() { return ++(*self); }
#define PREV(RET, CLASS) CLASS & prev() { return --(*self); }
#elif defined SWIGJAVA
#define NEXT(RET, CLASS) RET next() { return *(++(*self)); }
#define PREV(RET, CLASS) RET prev() { return *(--(*self)); }
#else
#define NEXT(RET, CLASS) void next() { ++(*self); }
#define PREV(RET, CLASS) void prev() { --(*self); }
#endif

#ifndef SWIGPYTHON
#ifdef SWIGCSHARP
%rename(Apply) operator();
#else
%rename(apply) operator();
#endif
#endif

namespace Xapian {

class Weight;
class Query;

}

%include xapian-headers.i

namespace Xapian {

// from xapian/enquire.h:

class MSetIterator;

class MSet {
  public:
    MSet();
    MSet(const MSet& other);
    ~MSet();

    void fetch(MSetIterator& begin, MSetIterator& end) const;
    void fetch(MSetIterator& item) const;
    void fetch() const;

    percent convert_to_percent(weight wt) const;
    percent convert_to_percent(const MSetIterator & item) const;

    doccount get_termfreq(std::string tname) const;
    weight get_termweight(std::string tname) const;
    doccount get_firstitem() const;
    doccount get_matches_lower_bound() const;
    doccount get_matches_estimated() const;
    doccount get_matches_upper_bound() const;
    doccount get_uncollapsed_matches_lower_bound() const;
    doccount get_uncollapsed_matches_estimated() const;
    doccount get_uncollapsed_matches_upper_bound() const;
    weight get_max_possible();
    weight get_max_attained();
    doccount size() const;
    bool empty() const;
    MSetIterator begin() const;
    MSetIterator end() const;
    MSetIterator back() const;
    %extend {
	MSetIterator get_hit(doccount i) const {
	    return ((*self)[i]);
	}
	int get_document_percentage(doccount i) const {
	    return (*self).convert_to_percent( ((*self)[i]) );
	}
	const Document get_document(doccount i) const {
	    return ((*self)[i]).get_document();
	}
	docid get_docid(doccount i) const {
	    return *((*self)[i]);
	}
#ifdef SWIGJAVA
	// For compatibility with the original JNI wrappers.
	MSetIterator getElement(doccount i) const {
	    return ((*self)[i]);
	}
#endif
    }
    string get_description() const;
};

class MSetIterator {
  public:
    MSetIterator();
    MSetIterator(const MSetIterator& other);
    ~MSetIterator();
    %extend {
	docid get_docid() const {
	    return *(*self);
	}
	NEXT(docid, MSetIterator)
	PREV(docid, MSetIterator)
	bool equals(const MSetIterator &other) const {
	    return (*self) == other;
	}
    }
    Document get_document() const;
    doccount get_rank() const;
    weight get_weight() const;
    string get_collapse_key() const;
    doccount get_collapse_count() const;
    percent get_percent() const;
    string get_description() const;
};

class ESetIterator;

class ESet {
  public:
    ESet();
    ESet(const ESet& other);
    ~ESet();
    termcount get_ebound() const;
    termcount size() const;
    bool empty() const;
    ESetIterator begin() const;
    ESetIterator end() const;
    ESetIterator back() const;
    string get_description() const;
};

class ESetIterator {
  public:
    ESetIterator();
    ESetIterator(const ESetIterator& other);
    ~ESetIterator();
    %extend {
	std::string get_term() const {
	    return *(*self);
	}
	NEXT(std::string, ESetIterator)
	PREV(std::string, ESetIterator)
	bool equals(const ESetIterator &other) const {
	    return (*self) == other;
	}
    }
    weight get_weight() const;
    string get_description() const;
};

class RSet {
  public:
    RSet(const RSet& other);
    RSet();
    ~RSet();
    doccount size() const;
    bool empty() const;
    void add_document(docid did);
    void add_document(MSetIterator& i);
    void remove_document(docid did);
    void remove_document(MSetIterator& i);
    bool contains(docid did);
    bool contains(MSetIterator& i);
    string get_description() const;
};

%feature("director") MatchDecider;
class MatchDecider {
  public:
    virtual bool operator() (const Xapian::Document &doc) const = 0;
    virtual ~MatchDecider();
};

/* MatchDecider and ExpandDecider are abstract classes, each only useful if it
 * can be subclassed.  There are some C++ subclasses of MatchDecider in core
 * xapian, but none for ExpandDecider: therefore ExpandDecider requires that
 * directors be supported.  So we only wrap ExpandDecider for languages which
 * support directors. */

#ifdef XAPIAN_SWIG_DIRECTORS
%feature("director") ExpandDecider;
class ExpandDecider {
  public:
    virtual bool operator() (const string &term) const = 0;
    virtual ~ExpandDecider();
};
#endif

}

#ifdef XAPIAN_TERMITERATOR_PAIR_OUTPUT_TYPEMAP
%template() std::pair<Xapian::TermIterator, Xapian::TermIterator>;
#endif

namespace Xapian {

class Enquire {
  public:
    Enquire(const Database &databases);
    ~Enquire();

    void set_query(const Query & query, termcount qlen = 0);
    const Query& get_query();

    void add_matchspy(MatchSpy * spy);
    void clear_matchspies();

    void set_weighting_scheme(const Weight& weight);
    void set_collapse_key(Xapian::valueno collapse_key,
                          Xapian::doccount collapse_max = 1);

    typedef enum {
	ASCENDING = 1,
	DESCENDING = 0,
	DONT_CARE = 2
    } docid_order;

    void set_docid_order(docid_order order);

    void set_cutoff(int percent_cutoff, weight weight_cutoff = 0);

    void set_sort_by_relevance();
    void set_sort_by_value(Xapian::valueno sort_key, bool reverse);
    void set_sort_by_value_then_relevance(Xapian::valueno sort_key,
					  bool reverse);
    void set_sort_by_relevance_then_value(Xapian::valueno sort_key,
					  bool reverse);
    void set_sort_by_key(Xapian::KeyMaker * sorter, bool reverse);
    void set_sort_by_key_then_relevance(Xapian::KeyMaker * sorter,
                                        bool reverse);
    void set_sort_by_relevance_then_key(Xapian::KeyMaker * sorter,
                                        bool reverse);

    static const int INCLUDE_QUERY_TERMS = 1;
    static const int USE_EXACT_TERMFREQ = 2;

    MSet get_mset(doccount first,
		  doccount maxitems,
		  doccount checkatleast = 0,
		  const RSet * omrset = 0,
		  const MatchDecider * mdecider = 0) const;
    MSet get_mset(doccount first,
		  doccount maxitems,
		  const RSet *omrset,
		  const MatchDecider *mdecider = 0) const;

#ifdef XAPIAN_SWIG_DIRECTORS
    ESet get_eset(termcount maxitems,
	    const RSet &omrset,
	    int flags = 0, double k = 1.0,
	    const ExpandDecider *edecider = 0) const;
    ESet get_eset(termcount maxitems,
	    const RSet & omrset,
	    int flags,
	    double k,
	    const ExpandDecider *edecider,
	    Xapian::weight min_wt) const;
    ESet get_eset(termcount maxitems, const RSet & omrset, const Xapian::ExpandDecider * edecider) const;
#else
    ESet get_eset(termcount maxitems,
	    const RSet &omrset,
	    int flags = 0, double k = 1.0) const;
#endif

    TermIterator get_matching_terms_begin(docid did) const;
    TermIterator get_matching_terms_end(docid did) const;
    TermIterator get_matching_terms_begin(const MSetIterator& i) const;
    TermIterator get_matching_terms_end(const MSetIterator& i) const;

#ifdef XAPIAN_TERMITERATOR_PAIR_OUTPUT_TYPEMAP
    /* We've not written the required custom typemap for all languages yet. */
    %extend {
	std::pair<Xapian::TermIterator, Xapian::TermIterator>
	get_matching_terms(const MSetIterator &hit) const {
	    return make_pair(self->get_matching_terms_begin(hit),
			     self->get_matching_terms_end(hit));
	}
    }
#endif

    string get_description() const;
};

}

/* Generated code won't compile if directors are enabled.  Disable for now
 * while we investigate.
 *
 * The problem comes from having a private pure virtual clone() function in
 * the Weight class. Directors work by multiple inheritance from both
 * SWIG_Director and the class they're directing; constructors in the target
 * language are then redirected to the director class. However the director
 * mechanism doesn't generate a wrapper for the clone() function (presumably
 * because it's private). This is wrong, because the director is then
 * abstract, which the SWIG generated code can't cope with.
 *
 * Also having a factory method might be a problem?
 */

//%feature("director") Weight;
%ignore Xapian::Weight::Internal;
%ignore Xapian::Weight::operator=;
%ignore Xapian::Weight::Weight(const Weight &);
%ignore Xapian::Weight::clone;
%ignore Xapian::Weight::serialise;
%ignore Xapian::Weight::unserialise;
%ignore Xapian::Weight::clone_;
%ignore Xapian::Weight::init_;
%include <xapian/weight.h>

namespace Xapian {

// xapian/dbfactory.h

// Database factory functions:
#if !defined SWIGCSHARP && !defined SWIGJAVA
namespace Auto {
    Database open_stub(const string & file);
}

namespace Brass {
    %rename(brass_open) open;
    Database open(const std::string &dir);
/* SWIG Tcl wrappers don't call destructors for classes returned by factory
 * functions, so don't wrap them so users are forced to use the
 * WritableDatabase ctor instead. */
#ifndef SWIGTCL
    WritableDatabase open(const std::string &dir, int action, int block_size = 8192);
#endif
}

namespace Chert {
    %rename(chert_open) open;
    Database open(const std::string &dir);
/* SWIG Tcl wrappers don't call destructors for classes returned by factory
 * functions, so don't wrap them so users are forced to use the
 * WritableDatabase ctor instead. */
#ifndef SWIGTCL
    WritableDatabase open(const std::string &dir, int action, int block_size = 8192);
#endif
}

namespace InMemory {
    %rename(inmemory_open) open;
    WritableDatabase open();
}

namespace Remote {
    %rename(remote_open) open;
    %rename(remote_open_writable) open_writable;

    Database open(const std::string &host, unsigned int port, Xapian::timeout timeout, Xapian::timeout connect_timeout);
    Database open(const std::string &host, unsigned int port, Xapian::timeout timeout = 10000);

    WritableDatabase open_writable(const std::string &host, unsigned int port, Xapian::timeout timeout, Xapian::timeout connect_timeout);
    WritableDatabase open_writable(const std::string &host, unsigned int port, Xapian::timeout timeout = 10000);

    Database open(const std::string &program, const std::string &args, Xapian::timeout timeout = 10000);

    WritableDatabase open_writable(const std::string &program, const std::string &args, Xapian::timeout timeout = 10000);
}
#else
/* Lie to SWIG that Auto, etc are classes with static methods rather than
   namespaces so it wraps it as we want in C# and Java. */
class Auto {
  private:
    Auto();
    ~Auto();
  public:
    static
    Database open_stub(const string & file);
};

class Brass {
  private:
    Brass();
    ~Brass();
  public:
    static
    Database open(const std::string &dir);
    static
    WritableDatabase open(const std::string &dir, int action, int block_size = 8192);
};

class Chert {
  private:
    Chert();
    ~Chert();
  public:
    static
    Database open(const std::string &dir);
    static
    WritableDatabase open(const std::string &dir, int action, int block_size = 8192);
};

class InMemory {
  private:
    InMemory();
    ~InMemory();
  public:
    static
    WritableDatabase open();
};

class Remote {
  private:
    Remote();
    ~Remote();
  public:
    static
    Database open(const std::string &host, unsigned int port, Xapian::timeout timeout, Xapian::timeout connect_timeout);
    static
    Database open(const std::string &host, unsigned int port, Xapian::timeout timeout = 10000);

    static
    WritableDatabase open_writable(const std::string &host, unsigned int port, Xapian::timeout timeout, Xapian::timeout connect_timeout);
    static
    WritableDatabase open_writable(const std::string &host, unsigned int port, Xapian::timeout timeout = 10000);

    static
    Database open(const std::string &program, const std::string &args, Xapian::timeout timeout = 10000);

    static
    WritableDatabase open_writable(const std::string &program, const std::string &args, Xapian::timeout timeout = 10000);
};
#endif

}

// xapian/query.h:

#if !defined SWIGTCL && !defined SWIGLUA
// FIXME: wrap MatchAll and MatchNothing for other languages (except for Python, Ruby, and Perl
// which wrap them in a different way)
%ignore Xapian::Query::MatchAll;
%ignore Xapian::Query::MatchNothing;
#endif

%ignore Xapian::Query::internal;
%ignore Xapian::Query::operator=;
%extend Xapian::Query {
#ifndef XAPIAN_MIXED_VECTOR_QUERY_INPUT_TYPEMAP
	    /* For some languages we handle strings in the vector<Query>
	     * case, so we don't need to wrap this ctor. */

	    /** Constructs a query from a vector of terms merged with the
	     *  specified operator. */
	    Query(Query::op op, const vector<string> & subqs, termcount param = 0) {
		return new Xapian::Query(op, subqs.begin(), subqs.end(), param);
	    }
#endif

	    /** Constructs a query from a vector of subqueries merged with the
	     *  specified operator. */
	    Query(Query::op op, const vector<Xapian::Query> & subqs, termcount param = 0) {
		return new Xapian::Query(op, subqs.begin(), subqs.end(), param);
	    }
}
%include <xapian/query.h>

%warnfilter(SWIGWARN_TYPE_UNDEFINED_CLASS) Xapian::StemImplementation;
#ifdef XAPIAN_SWIG_DIRECTORS
%feature("director") Xapian::StemImplementation;
#else
%ignore Xapian::StemImplementation;
%ignore Xapian::Stem::Stem(Xapian::StemImplementation *);
#endif
%ignore Xapian::Stem::internal;
%ignore Xapian::Stem::operator=;
%ignore Xapian::Stem::Stem();
%ignore Xapian::Stem::Stem(const Stem &);
%include <xapian/stem.h>

%include <xapian/valuesetmatchdecider.h>
