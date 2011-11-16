// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_TRACKED_OBJECTS_H_
#define BASE_TRACKED_OBJECTS_H_
#pragma once

#include <map>
#include <stack>
#include <string>
#include <vector>

#include "base/base_export.h"
#include "base/lazy_instance.h"
#include "base/location.h"
#include "base/profiler/tracked_time.h"
#include "base/time.h"
#include "base/synchronization/lock.h"
#include "base/threading/thread_local_storage.h"
#include "base/tracking_info.h"
#include "base/values.h"

// TrackedObjects provides a database of stats about objects (generally Tasks)
// that are tracked.  Tracking means their birth, death, duration, birth thread,
// death thread, and birth place are recorded.  This data is carefully spread
// across a series of objects so that the counts and times can be rapidly
// updated without (usually) having to lock the data, and hence there is usually
// very little contention caused by the tracking.  The data can be viewed via
// the about:profiler URL, with a variety of sorting and filtering choices.
//
// These classes serve as the basis of a profiler of sorts for the Tasks system.
// As a result, design decisions were made to maximize speed, by minimizing
// recurring allocation/deallocation, lock contention and data copying.  In the
// "stable" state, which is reached relatively quickly, there is no separate
// marginal allocation cost associated with construction or destruction of
// tracked objects, no locks are generally employed, and probably the largest
// computational cost is associated with obtaining start and stop times for
// instances as they are created and destroyed.
//
// The following describes the lifecycle of tracking an instance.
//
// First off, when the instance is created, the FROM_HERE macro is expanded
// to specify the birth place (file, line, function) where the instance was
// created.  That data is used to create a transient Location instance
// encapsulating the above triple of information.  The strings (like __FILE__)
// are passed around by reference, with the assumption that they are static, and
// will never go away.  This ensures that the strings can be dealt with as atoms
// with great efficiency (i.e., copying of strings is never needed, and
// comparisons for equality can be based on pointer comparisons).
//
// Next, a Births instance is created for use ONLY on the thread where this
// instance was created.  That Births instance records (in a base class
// BirthOnThread) references to the static data provided in a Location instance,
// as well as a pointer specifying the thread on which the birth takes place.
// Hence there is at most one Births instance for each Location on each thread.
// The derived Births class contains slots for recording statistics about all
// instances born at the same location.  Statistics currently include only the
// count of instances constructed.
//
// Since the base class BirthOnThread contains only constant data, it can be
// freely accessed by any thread at any time (i.e., only the statistic needs to
// be handled carefully, and stats are updated exclusively on the birth thread).
//
// For Tasks, having now either constructed or found the Births instance
// described above, a pointer to the Births instance is then recorded into the
// PendingTask structure in MessageLoop.  This fact alone is very useful in
// debugging, when there is a question of where an instance came from.  In
// addition, the birth time is also recorded and used to later evaluate the
// lifetime duration of the whole Task.  As a result of the above embedding, we
// can find out a Task's location of birth, and thread of birth, without using
// any locks, as all that data is constant across the life of the process.
//
// The above work *could* also be done for any other object as well by calling
// TallyABirthIfActive() and TallyRunOnNamedThreadIfTracking() as appropriate.
//
// The amount of memory used in the above data structures depends on how many
// threads there are, and how many Locations of construction there are.
// Fortunately, we don't use memory that is the product of those two counts, but
// rather we only need one Births instance for each thread that constructs an
// instance at a Location. In many cases, instances are only created on one
// thread, so the memory utilization is actually fairly restrained.
//
// Lastly, when an instance is deleted, the final tallies of statistics are
// carefully accumulated.  That tallying writes into slots (members) in a
// collection of DeathData instances.  For each birth place Location that is
// destroyed on a thread, there is a DeathData instance to record the additional
// death count, as well as accumulate the run-time and queue-time durations for
// the instance as it is destroyed (dies).  By maintaining a single place to
// aggregate this running sum *only* for the given thread, we avoid the need to
// lock such DeathData instances. (i.e., these accumulated stats in a DeathData
// instance are exclusively updated by the singular owning thread).
//
// With the above lifecycle description complete, the major remaining detail is
// explaining how each thread maintains a list of DeathData instances, and of
// Births instances, and is able to avoid additional (redundant/unnecessary)
// allocations.
//
// Each thread maintains a list of data items specific to that thread in a
// ThreadData instance (for that specific thread only).  The two critical items
// are lists of DeathData and Births instances.  These lists are maintained in
// STL maps, which are indexed by Location. As noted earlier, we can compare
// locations very efficiently as we consider the underlying data (file,
// function, line) to be atoms, and hence pointer comparison is used rather than
// (slow) string comparisons.
//
// To provide a mechanism for iterating over all "known threads," which means
// threads that have recorded a birth or a death, we create a singly linked list
// of ThreadData instances. Each such instance maintains a pointer to the next
// one.  A static member of ThreadData provides a pointer to the first item on
// this global list, and access via that all_thread_data_list_head_ item
// requires the use of the list_lock_.
// When new ThreadData instances is added to the global list, it is pre-pended,
// which ensures that any prior acquisition of the list is valid (i.e., the
// holder can iterate over it without fear of it changing, or the necessity of
// using an additional lock.  Iterations are actually pretty rare (used
// primarilly for cleanup, or snapshotting data for display), so this lock has
// very little global performance impact.
//
// The above description tries to define the high performance (run time)
// portions of these classes.  After gathering statistics, calls instigated
// by visiting about:profiler will assemble and aggregate data for display.  The
// following data structures are used for producing such displays.  They are
// not performance critical, and their only major constraint is that they should
// be able to run concurrently with ongoing augmentation of the birth and death
// data.
//
// For a given birth location, information about births is spread across data
// structures that are asynchronously changing on various threads.  For display
// purposes, we need to construct Snapshot instances for each combination of
// birth thread, death thread, and location, along with the count of such
// lifetimes.  We gather such data into a Snapshot instances, so that such
// instances can be sorted and aggregated (and remain frozen during our
// processing).  Snapshot instances use pointers to constant portions of the
// birth and death datastructures, but have local (frozen) copies of the actual
// statistics (birth count, durations, etc. etc.).
//
// A DataCollector is a container object that holds a set of Snapshots. The
// statistics in a snapshot are gathered asynhcronously relative to their
// ongoing updates.  It is possible, though highly unlikely, that stats such
// as a 64bit counter could be incorrectly recorded by this process.  The
// advantage to having fast (non-atomic) updates of the data outweighs the
// minimal risk of a singular corrupt statistic snapshot (only the snapshot
// could be corrupt, not the underlying and ongoing statistic).  In constrast,
// pointer data that is accessed during snapshotting is completely invariant,
// and hence is perfectly acquired (i.e., no potential corruption, and no risk
// of a bad memory reference).
//
// After an array of Snapshots instances are collected into a DataCollector,
// they need to be prepared for displaying our output.  We currently implement a
// direct rendering to HTML, but we will soon have a JSON serialization as well.

// For direct HTML display, the data must be sorted, and possibly aggregated
// (example: how many threads are in a specific consecutive set of Snapshots?
// What was the total birth count for that set? etc.).  Aggregation instances
// collect running sums of any set of snapshot instances, and are used to print
// sub-totals in an about:profiler page.
//
// TODO(jar): I need to store DataCollections, and provide facilities for taking
// the difference between two gathered DataCollections.  For now, I'm just
// adding a hack that Reset()s to zero all counts and stats.  This is also
// done in a slighly thread-unsafe fashion, as the resetting is done
// asynchronously relative to ongoing updates (but all data is 32 bit in size).
// For basic profiling, this will work "most of the time," and should be
// sufficient... but storing away DataCollections is the "right way" to do this.
// We'll accomplish this via JavaScript storage of snapshots, and then we'll
// remove the Reset() methods.

class MessageLoop;

namespace tracked_objects {

//------------------------------------------------------------------------------
// For a specific thread, and a specific birth place, the collection of all
// death info (with tallies for each death thread, to prevent access conflicts).
class ThreadData;
class BASE_EXPORT BirthOnThread {
 public:
  BirthOnThread(const Location& location, const ThreadData& current);

  const Location location() const { return location_; }
  const ThreadData* birth_thread() const { return birth_thread_; }

 private:
  // File/lineno of birth.  This defines the essence of the task, as the context
  // of the birth (construction) often tell what the item is for.  This field
  // is const, and hence safe to access from any thread.
  const Location location_;

  // The thread that records births into this object.  Only this thread is
  // allowed to update birth_count_ (which changes over time).
  const ThreadData* const birth_thread_;

  DISALLOW_COPY_AND_ASSIGN(BirthOnThread);
};

//------------------------------------------------------------------------------
// A class for accumulating counts of births (without bothering with a map<>).

class BASE_EXPORT Births: public BirthOnThread {
 public:
  Births(const Location& location, const ThreadData& current);

  int birth_count() const { return birth_count_; }

  // When we have a birth we update the count for this BirhPLace.
  void RecordBirth() { ++birth_count_; }

  // When a birthplace is changed (updated), we need to decrement the counter
  // for the old instance.
  void ForgetBirth() { --birth_count_; }  // We corrected a birth place.

  // Hack to quickly reset all counts to zero.
  void Clear() { birth_count_ = 0; }

 private:
  // The number of births on this thread for our location_.
  int birth_count_;

  DISALLOW_COPY_AND_ASSIGN(Births);
};

//------------------------------------------------------------------------------
// Basic info summarizing multiple destructions of a tracked object with a
// single birthplace (fixed Location).  Used both on specific threads, and also
// in snapshots when integrating assembled data.

class BASE_EXPORT DeathData {
 public:
  // Default initializer.
  DeathData() : count_(0) {}

  // When deaths have not yet taken place, and we gather data from all the
  // threads, we create DeathData stats that tally the number of births without
  // a corrosponding death.
  explicit DeathData(int count)
      : count_(count) {}

  // Update stats for a task destruction (death) that had a Run() time of
  // |duration|, and has had a queueing delay of |queue_duration|.
  void RecordDeath(DurationInt queue_duration,
                   DurationInt run_duration);

  // Metrics accessors.
  int count() const { return count_; }
  DurationInt run_duration() const { return run_time_.duration(); }
  DurationInt AverageMsRunDuration() const;
  DurationInt run_duration_max() const { return run_time_.max(); }
  DurationInt queue_duration() const { return queue_time_.duration(); }
  DurationInt AverageMsQueueDuration() const;
  DurationInt queue_duration_max() const { return queue_time_.max(); }

  // Accumulate metrics from other into this.  This method is never used on
  // realtime statistics, and only used in snapshots and aggregatinos.
  void AddDeathData(const DeathData& other);

  // Simple print of internal state for use in line of HTML.
  void WriteHTML(std::string* output) const;

  // Construct a DictionaryValue instance containing all our stats. The caller
  // assumes ownership of the returned instance.
  base::DictionaryValue* ToValue() const;

  // Reset all tallies to zero. This is used as a hack on realtime data.
  void Clear();

 private:
  // DeathData::Data is a helper class, useful when different metrics need to be
  // aggregated, such as queueing times, or run times.
  class Data {
   public:
    Data() : duration_(0), max_(0) {}
    ~Data() {}

    DurationInt duration() const { return duration_; }
    DurationInt max() const { return max_; }

    // Emits HTML formated description of members, assuming |count| instances
    // when calculating averages.
    void WriteHTML(int count, std::string* output) const;

    // Agggegate data into our state.
    void AddData(const Data& other);
    void AddDuration(DurationInt duration);

    // Central helper function for calculating averages (correctly, in only one
    // place).
    DurationInt AverageMsDuration(int count) const;

    // Resets all members to zero.
    void Clear();

   private:
    DurationInt duration_;  // Sum of all durations seen.
    DurationInt max_;       // Largest singular duration seen.
  };


  int count_;         // Number of deaths seen.
  Data run_time_;    // Data about run time durations.
  Data queue_time_;  // Data about queueing times durations.
};

//------------------------------------------------------------------------------
// A temporary collection of data that can be sorted and summarized.  It is
// gathered (carefully) from many threads.  Instances are held in arrays and
// processed, filtered, and rendered.
// The source of this data was collected on many threads, and is asynchronously
// changing.  The data in this instance is not asynchronously changing.

class BASE_EXPORT Snapshot {
 public:
  // When snapshotting a full life cycle set (birth-to-death), use this:
  Snapshot(const BirthOnThread& birth_on_thread, const ThreadData& death_thread,
           const DeathData& death_data);

  // When snapshotting a birth, with no death yet, use this:
  Snapshot(const BirthOnThread& birth_on_thread, int count);

  const ThreadData* birth_thread() const { return birth_->birth_thread(); }
  const Location location() const { return birth_->location(); }
  const BirthOnThread& birth() const { return *birth_; }
  const ThreadData* death_thread() const {return death_thread_; }
  const DeathData& death_data() const { return death_data_; }
  const std::string DeathThreadName() const;

  int count() const { return death_data_.count(); }
  DurationInt run_duration() const { return death_data_.run_duration(); }
  DurationInt AverageMsRunDuration() const {
    return death_data_.AverageMsRunDuration();
  }
  DurationInt run_duration_max() const {
    return death_data_.run_duration_max();
  }
  DurationInt queue_duration() const { return death_data_.queue_duration(); }
  DurationInt AverageMsQueueDuration() const {
    return death_data_.AverageMsQueueDuration();
  }
  DurationInt queue_duration_max() const {
    return death_data_.queue_duration_max();
  }

  // Construct a DictionaryValue instance containing all our data recursively.
  // The caller assumes ownership of the memory in the returned instance.
  base::DictionaryValue* ToValue() const;

 private:
  const BirthOnThread* birth_;  // Includes Location and birth_thread.
  const ThreadData* death_thread_;
  DeathData death_data_;
};

//------------------------------------------------------------------------------
// DataCollector is a container class for Snapshot and BirthOnThread count
// items.

class BASE_EXPORT DataCollector {
 public:
  typedef std::vector<Snapshot> Collection;

  // Construct with a list of how many threads should contribute.  This helps us
  // determine (in the async case) when we are done with all contributions.
  DataCollector();
  ~DataCollector();

  // Adds all stats from the indicated thread into our arrays.  This function
  // uses locks at the lowest level (when accessing the underlying maps which
  // could change when not locked), and can be called from any threads.
  void Append(const ThreadData& thread_data);

  // After the accumulation phase, the following accessor is used to process the
  // data (i.e., sort it, filter it, etc.).
  Collection* collection();

  // Adds entries for all the remaining living objects (objects that have
  // tallied a birth, but have not yet tallied a matching death, and hence must
  // be either running, queued up, or being held in limbo for future posting).
  // This should be called after all known ThreadData instances have been
  // processed using Append().
  void AddListOfLivingObjects();

  // Generates a ListValue representation of the vector of snapshots. The caller
  // assumes ownership of the memory in the returned instance.
  base::ListValue* ToValue() const;

 private:
  typedef std::map<const BirthOnThread*, int> BirthCount;

  // The array that we collect data into.
  Collection collection_;

  // The total number of births recorded at each location for which we have not
  // seen a death count.  This map changes as we do Append() calls, and is later
  // used by AddListOfLivingObjects() to gather up unaccounted for births.
  BirthCount global_birth_count_;

  DISALLOW_COPY_AND_ASSIGN(DataCollector);
};

//------------------------------------------------------------------------------
// For each thread, we have a ThreadData that stores all tracking info generated
// on this thread.  This prevents the need for locking as data accumulates.
// We use ThreadLocalStorage to quickly identfy the current ThreadData context.
// We also have a linked list of ThreadData instances, and that list is used to
// harvest data from all existing instances.

class BASE_EXPORT ThreadData {
 public:
  // Current allowable states of the tracking system.  The states can vary
  // between ACTIVE and DEACTIVATED, but can never go back to UNINITIALIZED.
  enum Status {
    UNINITIALIZED,
    ACTIVE,
    DEACTIVATED,
  };

  typedef std::map<Location, Births*> BirthMap;
  typedef std::map<const Births*, DeathData> DeathMap;

  // Initialize the current thread context with a new instance of ThreadData.
  // This is used by all threads that have names, and should be explicitly
  // set *before* any births on the threads have taken place.  It is generally
  // only used by the message loop, which has a well defined thread name.
  static void InitializeThreadContext(const std::string& suggested_name);

  // Using Thread Local Store, find the current instance for collecting data.
  // If an instance does not exist, construct one (and remember it for use on
  // this thread.
  // This may return NULL if the system is disabled for any reason.
  static ThreadData* Get();

  // Constructs a DictionaryValue instance containing all recursive results in
  // our process.  The caller assumes ownership of the memory in the returned
  // instance.
  static base::DictionaryValue* ToValue();

  // Finds (or creates) a place to count births from the given location in this
  // thread, and increment that tally.
  // TallyABirthIfActive will returns NULL if the birth cannot be tallied.
  static Births* TallyABirthIfActive(const Location& location);

  // Records the end of a timed run of an object.  The |completed_task| contains
  // a pointer to a Births, the time_posted, and a delayed_start_time if any.
  // The |start_of_run| indicates when we started to perform the run of the
  // task.  The delayed_start_time is non-null for tasks that were posted as
  // delayed tasks, and it indicates when the task should have run (i.e., when
  // it should have posted out of the timer queue, and into the work queue.
  // The |end_of_run| was just obtained by a call to Now() (just after the task
  // finished). It is provided as an argument to help with testing.
  static void TallyRunOnNamedThreadIfTracking(
      const base::TrackingInfo& completed_task,
      const TrackedTime& start_of_run,
      const TrackedTime& end_of_run);

  // Record the end of a timed run of an object.  The |birth| is the record for
  // the instance, the |time_posted| records that instant, which is presumed to
  // be when the task was posted into a queue to run on a worker thread.
  // The |start_of_run| is when the worker thread started to perform the run of
  // the task.
  // The |end_of_run| was just obtained by a call to Now() (just after the task
  // finished).
  static void TallyRunOnWorkerThreadIfTracking(
      const Births* birth,
      const TrackedTime& time_posted,
      const TrackedTime& start_of_run,
      const TrackedTime& end_of_run);

  // Record the end of execution in region, generally corresponding to a scope
  // being exited.
  static void TallyRunInAScopedRegionIfTracking(
      const Births* birth,
      const TrackedTime& start_of_run,
      const TrackedTime& end_of_run);

  const std::string thread_name() const { return thread_name_; }

  // ---------------------
  // TODO(jar):
  // The following functions should all be private, and are only public because
  // the collection is done externally.  We need to relocate that code from the
  // collection class into this class, and then all these methods can be made
  // private.
  // (Thread safe) Get start of list of all ThreadData instances.
  static ThreadData* first();
  // Iterate through the null terminated list of ThreadData instances.
  ThreadData* next() const { return next_; }
  // Using our lock, make a copy of the specified maps.  These calls may arrive
  // from non-local threads, and are used to quickly scan data from all threads
  // in order to build an HTML page for about:profiler.
  void SnapshotBirthMap(BirthMap *output) const;
  void SnapshotDeathMap(DeathMap *output) const;
  // -------- end of should be private methods.

  // Hack: asynchronously clear all birth counts and death tallies data values
  // in all ThreadData instances.  The numerical (zeroing) part is done without
  // use of a locks or atomics exchanges, and may (for int64 values) produce
  // bogus counts VERY rarely.
  static void ResetAllThreadData();

  // Initializes all statics if needed (this initialization call should be made
  // while we are single threaded). Returns false if unable to initialize.
  static bool Initialize();

  // Sets internal status_ to either become ACTIVE, or DEACTIVATED,
  // based on argument being true or false respectively.
  // If tracking is not compiled in, this function will return false.
  static bool InitializeAndSetTrackingStatus(bool status);
  static bool tracking_status();

  // Special versions of Now() for getting times at start and end of a tracked
  // run.  They are super fast when tracking is disabled, and have some internal
  // side effects when we are tracking, so that we can deduce the amount of time
  // accumulated outside of execution of tracked runs.
  static TrackedTime NowForStartOfRun();
  static TrackedTime NowForEndOfRun();

  // Provide a time function that does nothing (runs fast) when we don't have
  // the profiler enabled.  It will generally be optimized away when it is
  // ifdef'ed to be small enough (allowing the profiler to be "compiled out" of
  // the code).
  static TrackedTime Now();

 private:
  // Allow only tests to call ShutdownSingleThreadedCleanup.  We NEVER call it
  // in production code.
  friend class TrackedObjectsTest;

  typedef std::stack<const ThreadData*> ThreadDataPool;

  // Worker thread construction creates a name since there is none.
  ThreadData();
  // Message loop based construction should provide a name.
  explicit ThreadData(const std::string& suggested_name);

  ~ThreadData();

  // Push this instance to the head of all_thread_data_list_head_, linking it to
  // the previous head.  This is performed after each construction, and leaves
  // the instance permanently on that list.
  void PushToHeadOfList();

  // In this thread's data, record a new birth.
  Births* TallyABirth(const Location& location);

  // Find a place to record a death on this thread.
  void TallyADeath(const Births& birth,
                   DurationInt queue_duration,
                   DurationInt duration);

  // Using our lock to protect the iteration, Clear all birth and death data.
  void Reset();

  // This method is called by the TLS system when a thread terminates.
  // The argument may be NULL if this thread has never tracked a birth or death.
  static void OnThreadTermination(void* thread_data);

  // This method should be called when a worker thread terminates, so that we
  // can save all the thread data into a cache of reusable ThreadData instances.
  void OnThreadTerminationCleanup() const;

  // Cleans up data structures, and returns statics to near pristine (mostly
  // uninitialized) state.  If there is any chance that other threads are still
  // using the data structures, then the |leak| argument should be passed in as
  // true, and the data structures (birth maps, death maps, ThreadData
  // insntances, etc.) will be leaked and not deleted.  If you have joined all
  // threads since the time that InitializeAndSetTrackingStatus() was called,
  // then you can pass in a |leak| value of false, and this function will
  // delete recursively all data structures, starting with the list of
  // ThreadData instances.
  static void ShutdownSingleThreadedCleanup(bool leak);

  // We use thread local store to identify which ThreadData to interact with.
  static base::ThreadLocalStorage::Slot tls_index_;

  // Link to the most recently created instance (starts a null terminated list).
  // The list is traversed by about:profiler when it needs to snapshot data.
  // This is only accessed while list_lock_ is held.
  static ThreadData* all_thread_data_list_head_;
  // Set of ThreadData instances for use with worker threads. When a worker
  // thread is done (terminating), we push it into this pool.  When a new worker
  // thread is created, we first try to re-use a ThreadData instance from the
  // pool, and if none are available, construct a new one.
  // This is only accessed while list_lock_ is held.
  static ThreadDataPool* unregistered_thread_data_pool_;
  // The next available thread number.  This should only be accessed when the
  // list_lock_ is held.
  static int thread_number_counter_;
  // Incarnation sequence number, indicating how many times (during unittests)
  // we've either transitioned out of UNINITIALIZED, or into that state.  This
  // value is only accessed while the list_lock_ is held.
  static int incarnation_counter_;
  // Protection for access to all_thread_data_list_head_, and to
  // unregistered_thread_data_pool_.  This lock is leaked at shutdown.
  // The lock is very infrequently used, so we can afford to just make a lazy
  // instance and be safe.
  static base::LazyInstance<base::Lock,
    base::LeakyLazyInstanceTraits<base::Lock> > list_lock_;

  // Record of what the incarnation_counter_ was when this instance was created.
  // If the incarnation_counter_ has changed, then we avoid pushing into the
  // pool (this is only critical in tests which go through multiple
  // incarations).
  int incarnation_count_for_pool_;

  // We set status_ to SHUTDOWN when we shut down the tracking service.
  static Status status_;

  // Link to next instance (null terminated list). Used to globally track all
  // registered instances (corresponds to all registered threads where we keep
  // data).
  ThreadData* next_;

  // The name of the thread that is being recorded.  If this thread has no
  // message_loop, then this is a worker thread, with a sequence number postfix.
  std::string thread_name_;

  // Indicate if this is a worker thread, and the ThreadData contexts should be
  // stored in the unregistered_thread_data_pool_ when not in use.
  bool is_a_worker_thread_;

  // A map used on each thread to keep track of Births on this thread.
  // This map should only be accessed on the thread it was constructed on.
  // When a snapshot is needed, this structure can be locked in place for the
  // duration of the snapshotting activity.
  BirthMap birth_map_;

  // Similar to birth_map_, this records informations about death of tracked
  // instances (i.e., when a tracked instance was destroyed on this thread).
  // It is locked before changing, and hence other threads may access it by
  // locking before reading it.
  DeathMap death_map_;

  // Lock to protect *some* access to BirthMap and DeathMap.  The maps are
  // regularly read and written on this thread, but may only be read from other
  // threads.  To support this, we acquire this lock if we are writing from this
  // thread, or reading from another thread.  For reading from this thread we
  // don't need a lock, as there is no potential for a conflict since the
  // writing is only done from this thread.
  mutable base::Lock lock_;

  DISALLOW_COPY_AND_ASSIGN(ThreadData);
};

//------------------------------------------------------------------------------
// Provide simple way to to start global tracking, and to tear down tracking
// when done.  The design has evolved to *not* do any teardown (and just leak
// all allocated data structures).  As a result, we don't have any code in this
// destructor, and perhaps this whole class should go away.

class BASE_EXPORT AutoTracking {
 public:
  AutoTracking() {
    ThreadData::Initialize();
  }

  ~AutoTracking() {
    // TODO(jar): Consider emitting a CSV dump of the data at this point.  This
    // should be called after the message loops have all terminated (or at least
    // the main message loop is gone), so there is little chance for additional
    // tasks to be Run.
  }

 private:

  DISALLOW_COPY_AND_ASSIGN(AutoTracking);
};

}  // namespace tracked_objects

#endif  // BASE_TRACKED_OBJECTS_H_
