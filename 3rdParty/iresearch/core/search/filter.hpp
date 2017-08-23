//
// IResearch search engine 
// 
// Copyright (c) 2016 by EMC Corporation, All Rights Reserved
// 
// This software contains the intellectual property of EMC Corporation or is licensed to
// EMC Corporation from third parties. Use of this software and the intellectual property
// contained therein is expressly limited to the terms and conditions of the License
// Agreement under which it is provided by or on behalf of EMC.
// 

#ifndef IRESEARCH_QUERY_H
#define IRESEARCH_QUERY_H

#include "shared.hpp"
#include "sort.hpp"
#include "index/iterators.hpp"
#include "utils/memory.hpp"
#include "utils/string.hpp"
#include "utils/type_id.hpp"
#include "utils/attributes_provider.hpp"

#include <unordered_map>
#include <vector>
#include <boost/iterator/iterator_facade.hpp>

NS_ROOT

//////////////////////////////////////////////////////////////////////////////
/// @class score
/// @brief represents a score related for the particular document
//////////////////////////////////////////////////////////////////////////////
class IRESEARCH_API score : public attribute {
 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief applies score to the specified attributes collection ("src")
  /// @return pointer to the attribute
  //////////////////////////////////////////////////////////////////////////////
  static score* apply(attribute_store& src, const order::prepared& ord) {
    if (ord.empty()) {
      return nullptr;
    }

    auto& attr = src.emplace<score>();

    attr->order_ = &ord;
    attr->value_.resize(ord.size());
    attr->clear();

    return attr.get();
  }

  DECLARE_ATTRIBUTE_TYPE();
  DECLARE_FACTORY_DEFAULT();

  score();

  template<typename T>
  const T& get(size_t i) const {
    assert(order_ && sizeof(T) == (*order_)[i].bucket->size());
    return reinterpret_cast<const T&>(*(value_.c_str() + (*order_)[i].offset));
  }

  byte_type* leak() {
    return &(value_[0]);
  };

  const byte_type* c_str() const {
    return value_.c_str();
  }

  const bstring& value() const {
    return value_;
  }

  void clear() {
    if (order_) {
      for (auto& ord: *order_) {
        ord.bucket->prepare_score(&(value_[0]) + ord.offset);
      }
    }
  }

 private:
  IRESEARCH_API_PRIVATE_VARIABLES_BEGIN
  const order::prepared* order_;
  bstring value_;
  IRESEARCH_API_PRIVATE_VARIABLES_END
}; // score

template<typename State>
class states_cache {
public:
  explicit states_cache(size_t size) {
    states_.reserve(size);
  }

  states_cache(states_cache&& rhs) NOEXCEPT
    : states_(std::move(rhs.states_)) {
  }

  states_cache& operator=(states_cache&& rhs) NOEXCEPT {
    if (this != &rhs) {
      states_ = std::move(rhs.states_);
    }
    return *this;
  }

  State& insert(const sub_reader& rdr) {
    auto it = states_.emplace(&rdr, State()).first;
    return it->second;    
  }

  const State* find(const sub_reader& rdr) const {
    auto it = states_.find(&rdr);
    return states_.end() == it ? nullptr : &(it->second);
  }

  bool empty() const { return states_.empty(); }

private:
  typedef std::unordered_map<
    const sub_reader*, State
  > states_map_t;

  states_map_t states_;
}; // states_cache

struct index_reader;

////////////////////////////////////////////////////////////////////////////////
/// @class filter
/// @brief base class for all user-side filters
////////////////////////////////////////////////////////////////////////////////
class IRESEARCH_API filter {
 public:
  typedef iresearch::boost::boost_t boost_t;

  //////////////////////////////////////////////////////////////////////////////
  /// @class query
  /// @brief base class for all prepared(compiled) queries
  //////////////////////////////////////////////////////////////////////////////
  class IRESEARCH_API prepared: public util::attribute_store_provider {
   public:
    DECLARE_SPTR(prepared);
    DECLARE_FACTORY(prepared);

    static prepared::ptr empty();

    prepared() = default;
    explicit prepared(attribute_store&& attrs);
    virtual ~prepared();

    score_doc_iterator::ptr execute(const sub_reader& rdr) const {
      return execute(rdr, order::prepared::unordered());
    }

    using util::attribute_store_provider::attributes;
    virtual attribute_store& attributes() NOEXCEPT override final {
      return attrs_;
    }

    virtual score_doc_iterator::ptr execute(
      const sub_reader& rdr,
      const order::prepared& ord) const = 0;

   protected:
    friend class filter;

   private:
    attribute_store attrs_;
  }; // prepared

  DECLARE_PTR(filter);
  DECLARE_FACTORY(filter);

  filter(const type_id& type);
  virtual ~filter();

  virtual size_t hash() const {
    return std::hash<const type_id*>()(type_);
  }

  bool operator==(const filter& rhs) const {
    return equals( rhs );
  }

  bool operator!=( const filter& rhs ) const {
    return !( *this == rhs );
  }

  /* boost::hash_combile support */
  friend size_t hash_value( const filter& q ) {
    return q.hash();
  }

  /* boost - external boost */
  virtual filter::prepared::ptr prepare(
      const index_reader& rdr,
      const order::prepared& ord,
      boost_t boost) const = 0;

  filter::prepared::ptr prepare(
      const index_reader& rdr,
      const order::prepared& ord) const {
    return prepare(rdr, ord, boost::no_boost());
  }

  filter::prepared::ptr prepare(const index_reader& rdr) const {
    return prepare(rdr, order::prepared::unordered());
  }

  boost_t boost() const { return boost_; }

  filter& boost( boost_t boost ) {
    boost_ = boost;
    return *this;
  }

  const type_id& type() const { return *type_; }

 protected:
  virtual bool equals( const filter& rhs ) const {
    return type_ == rhs.type_;
  }

 private:
  boost_t boost_;
  const type_id* type_;
}; // filter

#define DECLARE_FILTER_TYPE() DECLARE_TYPE_ID(iresearch::type_id)
#define DEFINE_FILTER_TYPE(class_name) DEFINE_TYPE_ID(class_name,iresearch::type_id) { \
  static iresearch::type_id type; \
  return type; }

////////////////////////////////////////////////////////////////////////////////
/// @class all
/// @brief filter that returns all documents
////////////////////////////////////////////////////////////////////////////////
class IRESEARCH_API all: public filter {
 public:
  DECLARE_FILTER_TYPE();
  DECLARE_FACTORY_DEFAULT();

  all();

  virtual filter::prepared::ptr prepare(
    const index_reader& rdr,
    const order::prepared& ord,
    boost_t) const override;
}; // all

////////////////////////////////////////////////////////////////////////////////
/// @class empty
/// @brief filter that returns no documents
////////////////////////////////////////////////////////////////////////////////
class IRESEARCH_API empty: public filter {
 public:
  DECLARE_FILTER_TYPE();
  DECLARE_FACTORY_DEFAULT();

  empty();

  virtual filter::prepared::ptr prepare(
    const index_reader& rdr,
    const order::prepared& ord,
    boost_t
  ) const override;
};

NS_END

NS_BEGIN( std )

template<>
struct hash<iresearch::filter> {
  typedef iresearch::filter argument_type;
  typedef size_t result_type;

  result_type operator()( const argument_type& key) const {
    return key.hash();
  }
};

NS_END // std

#endif