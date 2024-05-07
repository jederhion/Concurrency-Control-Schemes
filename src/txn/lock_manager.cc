#include "lock_manager.h"
#include <algorithm>

LockManagerA::LockManagerA(deque<Txn*>* ready_txns) { ready_txns_ = ready_txns; }

bool LockManagerA::WriteLock(Txn* txn, const Key& key)
{
    //
    // Implement this method!

    //Create a new LockRequest object for the transaction
    //std::cout << "A WriteLock start\n";
    LockRequest lock_request(EXCLUSIVE, txn);    

    //Check if the key exists in the lock table. If it doesn’t, this means no other transaction holds a lock on the key
    if (lock_table_.find(key) == lock_table_.end()) {
        // Key does not exist in lock table
        //create a new deque, add the lock request to it, and add the deque to the lock table. 
        //Then, return true because the lock can be immediately granted.
        lock_table_[key] = new deque<LockRequest>();
        lock_table_[key]->push_back(lock_request);
        //std::cout << "A WriteLock end\n";
        return true;
    }   
    else {
        lock_table_[key]->push_back(lock_request);
        txn_waits_[txn]++;
        //std::cout << "A WriteLock end\n";
        return false;
    }
}

bool LockManagerA::ReadLock(Txn* txn, const Key& key)
{
    // Since Part 1A implements ONLY exclusive locks, calls to ReadLock can
    // simply use the same logic as 'WriteLock'.
    return WriteLock(txn, key);
}

void LockManagerA::Release(Txn* txn, const Key& key) {
    if (lock_table_.find(key) == lock_table_.end()) {
        return;  // Key not found or deque is null
    }

    // Check if the transaction is in the deque and remove it
    Txn* next_txn;
    for (std::deque<LockRequest>::iterator it = lock_table_[key]->begin(); it != lock_table_[key]->end(); ++it) {
        if (it->txn_ == txn && it->mode_ == EXCLUSIVE) {
            it = lock_table_[key]->erase(it);
            
            for (std::deque<LockRequest>::iterator request = lock_table_[key]->begin(); request != lock_table_[key]->end(); ++request) {
                next_txn = request->txn_;
                if (--txn_waits_[next_txn] == 0) {
                    txn_waits_.erase(next_txn);
                    ready_txns_->push_back(next_txn);
                    
                }
                if (request->mode_ == EXCLUSIVE) {
                    break;
                }
            }

            if (lock_table_[key]->empty()) {
            // If the deque is now empty, remove the key from the lock table           
                lock_table_.erase(key);
            
            }        

            break;
        }
    }
}




// NOTE: The owners input vector is NOT assumed to be empty.
LockMode LockManagerA::Status(const Key& key, vector<Txn*>* owners) {
    if (lock_table_.find(key) == lock_table_.end()) {
        return UNLOCKED;
    }

    LockMode mode = lock_table_[key]->front().mode_;
    owners->clear();
    owners->push_back(lock_table_[key]->front().txn_);

    return mode;
}




LockManagerB::LockManagerB(deque<Txn*>* ready_txns) { ready_txns_ = ready_txns; }

bool LockManagerB::WriteLock(Txn* txn, const Key& key) {
    LockRequest lock_request(EXCLUSIVE, txn);

    // If the key is not in the lock table, create a new deque and add it to the lock table
    if (lock_table_.find(key) == lock_table_.end()) {
        lock_table_[key] = new deque<LockRequest>();
    }

    // If the key is locked by another transaction, enqueue the lock request and return false
    if (!lock_table_[key]->empty() && lock_table_[key]->front().txn_ != txn) {
        lock_table_[key]->push_back(lock_request);
        txn_waits_[txn]++;
        return false;
    }

    // Enqueue the lock request and grant the lock
    lock_table_[key]->push_back(lock_request);
    return true;
}

bool LockManagerB::ReadLock(Txn* txn, const Key& key) {
    LockRequest lock_request(SHARED, txn);

    // If the key is not in the lock table, create a new deque and add it to the lock table
    if (lock_table_.find(key) == lock_table_.end()) {
        lock_table_[key] = new deque<LockRequest>();
    }

    // If the key is locked by another transaction, enqueue the lock request and return false
    if (!lock_table_[key]->empty() && lock_table_[key]->front().txn_ != txn) {
        lock_table_[key]->push_back(lock_request);
        txn_waits_[txn]++;
        return false;
    }

    // Enqueue the lock request and grant the lock
    lock_table_[key]->push_back(lock_request);
    return true;
}



void LockManagerB::Release(Txn* txn, const Key& key) {
    if (lock_table_.find(key) == lock_table_.end()) {
        return;  // Key not found or deque is null
    }

    LockMode txn_lock_mode;
    bool is_last_shared = true;
    Txn* next_txn = nullptr;

    for (std::deque<LockRequest>::iterator it = lock_table_[key]->begin(); it != lock_table_[key]->end();) {
        if (it->txn_ == txn) {
            txn_lock_mode = it->mode_;
            if (txn_lock_mode == EXCLUSIVE) {
                it = lock_table_[key]->erase(it);  // Erase and move to the next element           
               
            } else if (txn_lock_mode == SHARED) {
                it = lock_table_[key]->erase(it);  // Erase and move to the next element

                // Check if any other shared lock requests from the same transaction exist
                is_last_shared = true;
                for (std::deque<LockRequest>::const_iterator lr = lock_table_[key]->begin(); lr != lock_table_[key]->end(); ++lr) {
                    if (lr->txn_ == txn && lr->mode_ == SHARED) {
                        is_last_shared = false;
                        break;
                    }
                }
               
            }
        } else {
            ++it;  // Move to the next element
        }
    }

    if ((txn_lock_mode == SHARED && is_last_shared) || txn_lock_mode == EXCLUSIVE) {
        for (std::deque<LockRequest>::iterator request = lock_table_[key]->begin(); request != lock_table_[key]->end(); ++request) {
            next_txn = request->txn_;
            if (--txn_waits_[next_txn] == 0) {
                txn_waits_.erase(next_txn);
                ready_txns_->push_back(next_txn);               
            }

            if (request->mode_ == EXCLUSIVE) {
                    break;
            }

        }

        if (lock_table_[key]->empty()) {
            // If the deque is now empty, remove the key from the lock table           
                lock_table_.erase(key);            
        }   
    }
}



LockMode LockManagerB::Status(const Key& key, vector<Txn*>* owners)
{
    // Check if the key is in the lock table
    auto it = lock_table_.find(key);
    if (it != lock_table_.end() && !it->second->empty()) {
        // Clear the owners vector to ensure it's empty before adding owners
        owners->clear();

        // Get the lock mode of the first lock request in the deque
        LockMode mode = it->second->front().mode_;

        // Add the appropriate transactions to the owners vector based on the lock mode
        for (const LockRequest& request : *it->second) {
            if (mode == EXCLUSIVE) {
                owners->push_back(request.txn_);
                break;
            } else if (request.mode_ == SHARED) {
                owners->push_back(request.txn_);
            } else {
                break; // Stop adding owners if an EXCLUSIVE lock request is encountered
            }
        }

        return mode;
    }

    // If the key is not in the lock table or the deque is empty, return UNLOCKED
    return UNLOCKED;
}


