#include "txn_processor.h"
#include <stdio.h>
#include <set>
#include <unordered_set>
#include <algorithm>  // Include this for std::set_intersection
#include <chrono>

#include "lock_manager.h"



// Thread & queue counts for StaticThreadPool initialization.
#define THREAD_COUNT 8

TxnProcessor::TxnProcessor(CCMode mode) : mode_(mode), tp_(THREAD_COUNT), next_unique_id_(1)
{
    if (mode_ == LOCKING_EXCLUSIVE_ONLY)
        lm_ = new LockManagerA(&ready_txns_);
    else if (mode_ == LOCKING)
        lm_ = new LockManagerB(&ready_txns_);

    // Create the storage
    if (mode_ == MVCC || mode_ == MVCC_SSI)
    {
        storage_ = new MVCCStorage();
    }
    else
    {
        storage_ = new Storage();
    }

    storage_->InitStorage();

    // Start 'RunScheduler()' running.

    pthread_attr_t attr;
    pthread_attr_init(&attr);

#if !defined(_MSC_VER) && !defined(__APPLE__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int i = 0; i < 7; i++)
    {
        CPU_SET(i, &cpuset);
    }
    pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpuset);
#endif

    pthread_t scheduler_;
    pthread_create(&scheduler_, &attr, StartScheduler, reinterpret_cast<void*>(this));

    stopped_          = false;
    scheduler_thread_ = scheduler_;
}

void* TxnProcessor::StartScheduler(void* arg)
{
    reinterpret_cast<TxnProcessor*>(arg)->RunScheduler();
    return NULL;
}

TxnProcessor::~TxnProcessor()
{
    // Wait for the scheduler thread to join back before destroying the object and its thread pool.
    stopped_ = true;
    pthread_join(scheduler_thread_, NULL);

    if (mode_ == LOCKING_EXCLUSIVE_ONLY || mode_ == LOCKING) delete lm_;

    delete storage_;
}

void TxnProcessor::NewTxnRequest(Txn* txn)
{
    // Atomically assign the txn a new number and add it to the incoming txn
    // requests queue.
    mutex_.Lock();
    txn->unique_id_ = next_unique_id_;
    next_unique_id_++;
    txn_requests_.Push(txn);
    mutex_.Unlock();
}

Txn* TxnProcessor::GetTxnResult()
{
    Txn* txn;
    while (!txn_results_.Pop(&txn))
    {
        // No result yet. Wait a bit before trying again (to reduce contention on
        // atomic queues).
        usleep(1);
    }
    return txn;
}

void TxnProcessor::RunScheduler()
{
    switch (mode_)
    {
        case SERIAL:
            RunSerialScheduler();
            break;
        case LOCKING:
            RunLockingScheduler();
            break;
        case LOCKING_EXCLUSIVE_ONLY:
            RunLockingScheduler();
            break;
        case OCC:
            RunOCCScheduler();
            break;
        case P_OCC:
            RunOCCParallelScheduler();
            break;
        case MVCC:
            RunMVCCScheduler();
    }
}

void TxnProcessor::RunSerialScheduler()
{
    Txn* txn;
    while (!stopped_)
    {
        // Get next txn request.
        if (txn_requests_.Pop(&txn))
        {
            // Execute txn.
            ExecuteTxn(txn);

            // Commit/abort txn according to program logic's commit/abort decision.
            if (txn->Status() == COMPLETED_C)
            {
                ApplyWrites(txn);
                committed_txns_.Push(txn);
                txn->status_ = COMMITTED;
            }
            else if (txn->Status() == COMPLETED_A)
            {
                txn->status_ = ABORTED;
            }
            else
            {
                // Invalid TxnStatus!
                DIE("Completed Txn has invalid TxnStatus: " << txn->Status());
            }

            // Return result to client.
            txn_results_.Push(txn);
        }
    }
}

void TxnProcessor::RunLockingScheduler()
{
    Txn* txn;
    while (!stopped_)
    {
        // Start processing the next incoming transaction request.
        if (txn_requests_.Pop(&txn))
        {
            bool blocked = false;
            // Request read locks.
            for (set<Key>::iterator it = txn->readset_.begin(); it != txn->readset_.end(); ++it)
            {
                if (!lm_->ReadLock(txn, *it))
                {
                    blocked = true;
                }
            }

            // Request write locks.
            for (set<Key>::iterator it = txn->writeset_.begin(); it != txn->writeset_.end(); ++it)
            {
                if (!lm_->WriteLock(txn, *it))
                {
                    blocked = true;
                }
            }

            // If all read and write locks were immediately acquired, this txn is
            // ready to be executed.
            if (blocked == false)
            {
                ready_txns_.push_back(txn);
            }
        }

        // Process and commit all transactions that have finished running.
        while (completed_txns_.Pop(&txn))
        {
            // Commit/abort txn according to program logic's commit/abort decision.
            if (txn->Status() == COMPLETED_C)
            {
                ApplyWrites(txn);
                committed_txns_.Push(txn);
                txn->status_ = COMMITTED;
            }
            else if (txn->Status() == COMPLETED_A)
            {
                txn->status_ = ABORTED;
            }
            else
            {
                // Invalid TxnStatus!
                DIE("Completed Txn has invalid TxnStatus: " << txn->Status());
            }

            // Release read locks.
            for (set<Key>::iterator it = txn->readset_.begin(); it != txn->readset_.end(); ++it)
            {
                lm_->Release(txn, *it);
            }
            // Release write locks.
            for (set<Key>::iterator it = txn->writeset_.begin(); it != txn->writeset_.end(); ++it)
            {
                lm_->Release(txn, *it);
            }

            // Return result to client.
            txn_results_.Push(txn);
        }

        // Start executing all transactions that have newly acquired all their
        // locks.
        while (ready_txns_.size())
        {
            // Get next ready txn from the queue.
            txn = ready_txns_.front();
            ready_txns_.pop_front();

            // Start txn running in its own thread.
            tp_.AddTask([this, txn]() { this->ExecuteTxn(txn); });
        }
    }
}

void TxnProcessor::ExecuteTxn(Txn* txn)
{
    // Get the current commited transaction index for the further validation.
    txn->occ_start_idx_ = committed_txns_.Size();

    // Read everything in from readset.
    for (set<Key>::iterator it = txn->readset_.begin(); it != txn->readset_.end(); ++it)
    {
        // Save each read result iff record exists in storage.
        Value result;
        if (storage_->Read(*it, &result)) txn->reads_[*it] = result;
    }

    // Also read everything in from writeset.
    for (set<Key>::iterator it = txn->writeset_.begin(); it != txn->writeset_.end(); ++it)
    {
        // Save each read result iff record exists in storage.
        Value result;
        if (storage_->Read(*it, &result)) txn->reads_[*it] = result;
    }

    // Execute txn's program logic.
    txn->Run();

    // Hand the txn back to the RunScheduler thread.
    completed_txns_.Push(txn);
}


void TxnProcessor::CleanupTxn(Txn* txn) {    
    txn->reads_.clear();
    txn->writes_.clear();
    txn->status_ = INCOMPLETE;
}

void TxnProcessor::RestartTxn(Txn* txn) {    
    mutex_.Lock();
    txn->unique_id_ = next_unique_id_;
    next_unique_id_++;
    txn_requests_.Push(txn);
    mutex_.Unlock();

}

void TxnProcessor::MVCCExecuteTxn(Txn* txn) {
    // Read all necessary data for this transaction from storage
    bool valid = true;
    for (const auto& key : txn->readset_) {

        storage_->Lock(key);
        valid = storage_->Read(key, &txn->reads_[key], txn->unique_id_);
        storage_->Unlock(key);

        if (!valid) {            
            CleanupTxn(txn);
            RestartTxn(txn);        
            return;
        }        
    }
    // Execute the transaction logic (i.e. call Run() on the transaction)
    txn->Run();
    
    // Acquire all locks for keys in the write_set_
    MVCCLockWriteKeys(txn);

    bool write_check_passed = false;

    //Call MVCCStorage::CheckWrite method to check all keys in the write_set_
    write_check_passed = MVCCCheckWrites(txn);
    
    if (write_check_passed) {
        // Apply the writes
        ApplyWrites(txn);
       
         // Commit the transaction.
        txn->status_ = COMMITTED;
        // Update relevant data structure.
        committed_txns_.Push(txn);
        txn_results_.Push(txn);

        // Release all locks for keys in the write_set_
        MVCCUnlockWriteKeys(txn);       
       
    } else {
        // Release all locks for keys in the write_set_
        MVCCUnlockWriteKeys(txn);
        CleanupTxn(txn);       
        RestartTxn(txn);
    }
}



 bool TxnProcessor::MVCCCheckWrites(Txn* txn)
 {
    bool write_check_passed = true;
    for (const auto& key : txn->writeset_) {
        write_check_passed = storage_->CheckKey(key, txn->unique_id_);
        if (!write_check_passed) {            
            return false;
        }        
    }
    return true;
 }

void TxnProcessor::MVCCLockWriteKeys(Txn* txn)
{
    for (const auto& key : txn->writeset_) {
        storage_->Lock(key);
    }  
}

void TxnProcessor::MVCCUnlockWriteKeys(Txn* txn)
{
    for (const auto& key : txn->writeset_) {
        storage_->Unlock(key);
    }
}


void TxnProcessor::ApplyWrites(Txn* txn)
{
    // Write buffered writes out to storage.
    for (map<Key, Value>::iterator it = txn->writes_.begin(); it != txn->writes_.end(); ++it)
    {
        storage_->Write(it->first, it->second, txn->unique_id_);
    }
}




void TxnProcessor::RunOCCScheduler() {
    while (!stopped_) {
        // Get the next new transaction request (if one is pending) and pass it to an execution thread.
        Txn* txn;
        if (txn_requests_.Pop(&txn)) {
            tp_.AddTask([this, txn]() {
                // Record OCC start index.
                //txn->occ_start_idx_ = committed_txns_.Size();

                // Perform "read phase" of transaction.
                ExecuteTxn(txn);

                //txn->Run();

                // Push the transaction to the completed transactions queue.
                //completed_txns_.Push(txn);
            });
        }

        // Deal with all transactions that have finished running.
        while (completed_txns_.Pop(&txn)) {
            // Validation phase.
            bool valid = true;
            for (int i = txn->occ_start_idx_; i < committed_txns_.Size(); i++) {
                Txn* committed_txn = committed_txns_[i];  // Use array subscript operator for AtomicVector.

                // Check for overlap in the read set.
                for (const Key& key : txn->readset_) {
                    if (committed_txn->writeset_.count(key) > 0) {
                        valid = false;
                        break;
                    }
                }

                // Check for overlap in the write set.
                if (valid) {
                    for (const Key& key : txn->writeset_) {
                        if (committed_txn->writeset_.count(key) > 0) {
                            valid = false;
                            break;
                        }
                    }
                }

                if (!valid) {
                    break;
                }
            }

            // Commit/restart.
            if (!valid) {
                CleanupTxn(txn);
                RestartTxn(txn);
            } else {
                // Apply all writes.
                ApplyWrites(txn);

                // Mark transaction as committed.
                txn->status_ = COMMITTED;

                // Update relevant data structure.
                committed_txns_.Push(txn);

                // Push the transaction to the results queue.
                txn_results_.Push(txn);
            }
        }
    }
}


void TxnProcessor::RunOCCParallelScheduler()
{
    while (!stopped_) {
        // Get the next new transaction request (if one is pending) and pass it to an execution thread.
        Txn* txn = nullptr;
        if (txn_requests_.Pop(&txn) && txn != nullptr) {
            tp_.AddTask([this, txn]() {
                // Record start time (index in this case)
               // txn->occ_start_idx_ = next_unique_id_;

                // Perform "read phase" of transaction:
                ExecuteTxn(txn);

                // Start of critical section
                active_set_mutex_.Lock();
                auto active_set_copy = active_set_.GetSet();  // Make a copy of the active set
                active_set_.Insert(txn);                      // Add this transaction to the active set
                active_set_mutex_.Unlock();
                // End of critical section

                // Do validation phase:
                bool valid = true;
                 for (const auto& t : active_set_copy) {
                    if (t == txn || t == nullptr) continue;  // Skip self and null transactions

                    // Check if txn's write set intersects with t's write sets
                    for (const auto& key : txn->writeset_) {
                        if (t->writeset_.count(key) > 0) {
                            valid = false;
                            break;
                        }
                    }
                    if (!valid) break;

                    // Check if txn's read set intersects with t's write sets
                    for (const auto& key : txn->readset_) {
                        if (t->writeset_.count(key) > 0) {
                            valid = false;
                            break;
                        }
                    }
                    if (!valid) break;
                }


                // Critical section to update active set and apply writes if valid
                //active_set_mutex_.Lock();
                //active_set_.Erase(txn);  // Remove this transaction from the active set
                if (valid) {
                    // Apply writes;
                    ApplyWrites(txn);

                    // Remove this transaction from the active set.
                    active_set_mutex_.Lock();
                    active_set_.Erase(txn);
                    active_set_mutex_.Unlock();
                    // Mark transaction as committed
                    txn->status_ = COMMITTED;
                    // Update relevant data structure
                    committed_txns_.Push(txn);

                     // Push the transaction result
                    txn_results_.Push(txn);
                } else {

                     // Remove this transaction from the active set.
                    active_set_mutex_.Lock();
                    active_set_.Erase(txn);
                    active_set_mutex_.Unlock();

                    CleanupTxn(txn);
                    RestartTxn(txn);
                }
               
            });
        }
    }
}




void TxnProcessor::RunMVCCScheduler()
{
    //
    // Implement this method!

    // Hint:Pop a txn from txn_requests_, and pass it to a thread to execute.
    // Note that you may need to create another execute method, like TxnProcessor::MVCCExecuteTxn.
    //
    // [For now, run serial scheduler in order to make it through the test
    // suite]
    //RunSerialScheduler();

     while (!stopped_) {
        Txn* txn;
        if (txn_requests_.Pop(&txn)) {
            tp_.AddTask([this, txn]() {
                // Lock, read, execute, validate, write, and potentially restart the transaction.
                this->MVCCExecuteTxn(txn);
            });
        }
    }
}

void TxnProcessor::RunMVCCSSIScheduler()
{
    //
    // Implement this method!

    // Hint:Pop a txn from txn_requests_, and pass it to a thread to execute.
    // Note that you may need to create another execute method, like TxnProcessor::MVCCSSIExecuteTxn.
    //
    // [For now, run serial scheduler in order to make it through the test
    // suite]
    //RunSerialScheduler();

      
    while (!stopped_) {
        Txn* txn;
        if (txn_requests_.Pop(&txn)) {
            tp_.AddTask([this, txn]() {
                this->MVCCSSIExecuteTxn(txn);
            });
        }
    }

}



void TxnProcessor::MVCCSSIExecuteTxn(Txn* txn) {

     // Read all necessary data for this transaction from storage
    bool valid = true;
    for (const auto& key : txn->readset_) {

        storage_->Lock(key);
        valid = storage_->Read(key, &txn->reads_[key], txn->unique_id_);
        storage_->Unlock(key);

        if (!valid) {            
            CleanupTxn(txn);
            RestartTxn(txn);        
            return;
        }        
    }
    // Execute the transaction logic (i.e. call Run() on the transaction)
    txn->Run();

    // Acquire all locks for keys in the read_set_ and write_set_ (Lock any overlapping key only once.)
    // Merge read and write sets to ensure each key is locked only once.
    std::unordered_set<Key> all_keys;
    all_keys.insert(txn->readset_.begin(), txn->readset_.end());
    all_keys.insert(txn->writeset_.begin(), txn->writeset_.end());
    
    for (const auto& key : all_keys) {
        storage_->Lock(key);
    }    
    

    //Call MVCCStorage::CheckKey method to check all keys
    bool check_passed = false;
    for (const auto& key : all_keys) {
        check_passed = storage_->CheckKey(key, txn->unique_id_);
        if (!check_passed) {            
            break;
        }        
    }   

 
    if (check_passed) {
        // Apply the writes
        ApplyWrites(txn);
       
         // Commit the transaction.
        txn->status_ = COMMITTED;
        // Update relevant data structure.
        committed_txns_.Push(txn);
        txn_results_.Push(txn);

        
        // Release all locks for all keys.
        for (const auto& key : all_keys) {
            storage_->Unlock(key);
        }     
       
    } else {
        // Release all locks for all keys.
        for (const auto& key : all_keys) {
            storage_->Unlock(key);
        }   
        CleanupTxn(txn);       
        RestartTxn(txn);
    }
   
}




