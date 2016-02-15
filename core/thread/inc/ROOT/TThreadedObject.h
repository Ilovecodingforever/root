// @(#)root/thread:$Id$
// Author: Danilo Piparo, CERN  11/2/2015

/*************************************************************************
 * Copyright (C) 1995-2016, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_TThreadedObject
#define ROOT_TThreadedObject

#ifndef ROOT_TList
#include "TList.h"
#endif

#ifndef ROOT_TError
#include "TError.h"
#endif

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace ROOT {

   namespace Internal {

      namespace TThreadedObjectUtils {

         /// Return a copy of the object or a "Clone" if the copy constructor is not implemented.
         template<class T, bool isCopyConstructible = std::is_copy_constructible<T>::value>
         struct Cloner {
            static T *Clone(const T &obj) {
               return new T(obj);
            }
         };

         template<class T>
         struct Cloner<T, false> {
            static T *Clone(const T &obj) {
               return (T*)obj.Clone();
            }
         };

      } // End of namespace TThreadedObjectUtils
   } // End of namespace Internals

   namespace TThreadedObjectUtils {

      template<class T>
      using MergeFunctionType = std::function<void(std::shared_ptr<T>, std::vector<std::shared_ptr<T>>&)>;
      /// Merge TObjects
      template<class T>
      void MergeTObjects(std::shared_ptr<T> target, std::vector<std::shared_ptr<T>> &objs)
      {
         if (!target) return;
         TList objTList;
         // Cannot do better than this
         for (auto obj : objs) {
            if (obj && obj != target) objTList.Add(obj.get());
         }
         target->Merge(&objTList);
      }
   } // end of namespace TThreadedObjectUtils

   /**
    * \class ROOT::TThreadedObject
    * \brief A wrapper to make object instances thread private, lazily.
    * \tparam T Class of the object to be made thread private (e.g. TH1F)
    * \ingroup Multicore
    *
    * A wrapper which makes objects thread private. The methods of the underlying
    * object can be invoked via the the arrow operator. The object is created in
    * a specific thread lazily, i.e. upon invocation of one of its methods.
    * The correct object pointer from within a particular thread can be accessed
    * with the overloaded arrow operator or with the Get method.
    * In case an elaborate thread management is in place, e.g. in presence of
    * stream of operations or "processing slots", it is also possible to
    * manually select the correct object pointer explicitly.
    */
   template<class T>
   class TThreadedObject {
   public:
      static unsigned fgMaxSlots; ///< The maximum number of processing slots (distinct threads) which the instances can manage
      /// Construct the TThreaded object and the "model" of the thread private
      /// objects.
      /// \tparam ARGS Arguments of the constructor of T
      template<class ...ARGS>
      TThreadedObject(ARGS... args):
      fModel(std::forward<ARGS>(args)...), fObjPointers(fgMaxSlots, nullptr) {};

      /// Access a particular processing slot. This
      /// method is *thread-unsafe*: it cannot be invoked from two different
      /// threads with the same argument.
      std::shared_ptr<T> GetAtSlot(unsigned i)
      {
         if ( i >= fObjPointers.size()) {
            Warning("TThreadedObject::Merge", "Maximum number of slots reached.");
            return nullptr;
         }
         auto objPointer = fObjPointers[i];
         if (!objPointer) {
            objPointer.reset(Internal::TThreadedObjectUtils::Cloner<T>::Clone(fModel));
            fObjPointers[i] = objPointer;
         }
         return objPointer;
      }

      /// Access a particular slot which corresponds to a single thread.
      /// This is in general faster than the GetAtSlot method but it is
      /// responsibility of the caller to make sure that an object is
      /// initialised for the particular slot.
      std::shared_ptr<T> GetAtSlotUnchecked(unsigned i) const
      {
         return fObjPointers[i];
      }

      /// Access the pointer corresponding to the current slot. This method is
      /// not adequate for being called inside tight loops as it implies a
      /// lookup in a mapping between the threadIDs and the slot indices.
      /// A good practice consists in copying the pointer onto the stack and
      /// proceed with the loop as shown in this work item (psudo-code) which
      /// will be sent to different threads:
      /// ~~~{.cpp}
      /// auto workItem = [](){
      ///    auto objPtr = tthreadedObject.GetAtThisSlot();
      ///    for (auto i : ROOT::TSeqI(1000)) {
      ///       // tthreadedObject->FastMethod(i); // don't do this! Inefficient!
      ///       objPtr->FastMethod(i);
      ///    }
      /// }
      /// ~~~
      std::shared_ptr<T> Get()
      {
         return GetAtSlot(GetThisSlotNumber());
      }

      /// Access the wrapped object and allow to call its methods.
      T *operator->()
      {
         return Get().get();
      }

      /// Merge all the thread private objects. Can be called once: it does not
      /// create any new object but destroys the present bookkeping collapsing
      /// all objects into the one at slot 0.
      std::shared_ptr<T> Merge(TThreadedObjectUtils::MergeFunctionType<T> mergeFunction = TThreadedObjectUtils::MergeTObjects<T>)
      {
         // We do not return if we already merged.
         if (fIsMerged) {
            Warning("TThreadedObject::Merge", "This object was already merged. Returning the previous result.");
            return fObjPointers[0];
         }
         mergeFunction(fObjPointers[0], fObjPointers);
         fIsMerged = true;
         return fObjPointers[0];
      }

      /// Merge all the thread private objects. Can be called many times. It
      /// does create a new instance of class T to represent the "Sum" object.
      /// This method is not thread safe: correct or acceptable behaviours
      /// depend on the nature of T and of the merging function.
      std::unique_ptr<T> SnapshotMerge(TThreadedObjectUtils::MergeFunctionType<T> mergeFunction = TThreadedObjectUtils::MergeTObjects<T>)
      {
         if (fIsMerged) {
            Warning("TThreadedObject::SnapshotMerge", "This object was already merged. Returning the previous result.");
            return std::unique_ptr<T>(Internal::TThreadedObjectUtils::Cloner<T>::Clone(*fObjPointers[0].get()));
         }
         auto targetPtr = Internal::TThreadedObjectUtils::Cloner<T>::Clone(fModel);
         std::shared_ptr<T> targetPtrShared(targetPtr, [](T *) {});
         mergeFunction(targetPtrShared, fObjPointers);
         return std::unique_ptr<T>(targetPtr);
      }

   private:
      const T fModel;                                    ///< Use to store a "model" of the object
      std::vector<std::shared_ptr<T>> fObjPointers;      ///< A pointer per thread is kept.
      std::map<std::thread::id, unsigned> fThrIDSlotMap; ///< A mapping between the thread IDs and the slots
      unsigned fCurrMaxSlotIndex = 0;                    ///< The maximum slot index
      bool fIsMerged = false;                            ///< Remember if the objects have been merged already
      std::mutex* fThrIDSlotMutex = new std::mutex;      ///< Mutex to protect the ID-slot map access

      /// Get the slot number for this threadID.
      unsigned GetThisSlotNumber()
      {
         const auto thisThreadID = std::this_thread::get_id();
         unsigned thisIndex;
         {
            std::lock_guard<std::mutex> lg(*fThrIDSlotMutex);
            auto thisSlotNumIt = fThrIDSlotMap.find(thisThreadID);
            if (thisSlotNumIt != fThrIDSlotMap.end()) return thisSlotNumIt->second;
            thisIndex = fCurrMaxSlotIndex++;
            fThrIDSlotMap[thisThreadID] = thisIndex;
         }
         return thisIndex;
      }

   };

   template<class T> unsigned TThreadedObject<T>::fgMaxSlots = 64;

   ////////////////////////////////////////////////////////////////////////////////
   /// Obtain a TThreadedObject instance
   /// \tparam T Class of the object to be made thread private (e.g. TH1F)
   /// \tparam ARGS Arguments of the constructor
   template<class T, class ...ARGS>
   TThreadedObject<T> MakeThreaded(ARGS &&... args)
   {
      return TThreadedObject<T>(std::forward<ARGS>(args)...);
   }

} // End ROOT namespace

#endif
