#include "mvcc_storage.h"

// Init the storage
void MVCCStorage::InitStorage()
{
    for (int i = 0; i < 1000000; i++)
    {
        Write(i, 0, 0);
        Mutex* key_mutex = new Mutex();
        mutexs_[i]       = key_mutex;
    }
}

// Free memory.
MVCCStorage::~MVCCStorage()
{
    for (auto it = mvcc_data_.begin(); it != mvcc_data_.end(); ++it)
    {
        delete it->second;
    }

    mvcc_data_.clear();

    for (auto it = mutexs_.begin(); it != mutexs_.end(); ++it)
    {
        delete it->second;
    }

    mutexs_.clear();
}

// Lock the key to protect its version_list. Remember to lock the key when you read/update the version_list
void MVCCStorage::Lock(Key key)
{
    mutexs_[key]->Lock();
}

// Unlock the key.
void MVCCStorage::Unlock(Key key)
{
    mutexs_[key]->Unlock();
}

// MVCC Read
bool MVCCStorage::Read(Key key, Value *result, int txn_unique_id)
{
    //
    // Implement this method!

    // Hint: Iterate the version_lists and return the verion whose write timestamp
    // (version_id) is the largest write timestamp less than or equal to txn_unique_id.
  

   //std::cout << "-------herr--------\n";


   // Check if the key exists in the storage
    if (mvcc_data_.find(key) == mvcc_data_.end())
    {   
       // std::cout << "-------1--------\n" << key;
        return false; // Key not found
    }

    // Get the version list for the key
    deque<Version*>* version_list = mvcc_data_[key];

    // Iterate over the version list
    for (auto it = version_list->rbegin(); it != version_list->rend(); ++it)
    {
        // Check if the version's write timestamp is less than or equal to txn_unique_id
        if ((*it)->version_id_ <= txn_unique_id)
        {
            // Set the result to the value of this version
            *result = (*it)->value_;

            // Update the max_read_id_ of this version
            (*it)->max_read_id_ = std::max((*it)->max_read_id_, txn_unique_id);

           // std::cout << "-------2--------\n";
            return true; // Read successful
        }
    }

   // std::cout << "-------3--------\n";
    return false; // No suitable version found
    
  
}

// Check whether the txn executed on the latest version of the key.
bool MVCCStorage::CheckKey(Key key, int txn_unique_id)
{
    //
    // Implement this method!

    // Hint: Before all writes are applied (and SSI reads are validated), we need 
    // to make sure that each key was accessed safely based on MVCC timestamp ordering
    // protocol. This method only checks one key, so you should call this method for
    // each key (as necessary). Return true if this key passes the check, return false if not.
    // Note that you don't have to call Lock(key) in this method, just
    // call Lock(key) before you call this method and call Unlock(key) afterward.
   


    // Check if the key exists in the storage
    if (mvcc_data_.find(key) == mvcc_data_.end())
    {
        return false; // Key not found
    }

    // Get the version list for the key
    deque<Version*>* version_list = mvcc_data_[key];

    // Check if the latest version's write timestamp is less than or equal to txn_unique_id
    if (!version_list->empty() && version_list->front()->version_id_ <= txn_unique_id)
    {
        return true; // This key passes the check
    }

    return false; // This key does not pass the check
        
    //return true;
}

// MVCC Write, call this method only if CheckWrite return true.
void MVCCStorage::Write(Key key, Value value, int txn_unique_id)
{
    //
    // Implement this method!

    // Hint: Insert a new version (malloc a Version and specify its value/version_id/max_read_id)
    // into the version_lists. Note that InitStorage() also calls this method to init storage.
    // Note that you don't have to call Lock(key) in this method, just
    // call Lock(key) before you call this method and call Unlock(key) afterward.
    // Note that the performance would be much better if you organize the versions in decreasing order.
   
    //std::cout << "--------1----------\n";
    // Create a new version
    Version* new_version = new Version;
    new_version->value_ = value;
    new_version->version_id_ = txn_unique_id;
    new_version->max_read_id_ = txn_unique_id;

    // Check if the key exists in the storage
    if (mvcc_data_.find(key) == mvcc_data_.end())
    {
        // If the key does not exist, create a new deque and insert it into the map
        mvcc_data_[key] = new deque<Version*>;
    }

    // Get the version list for the key
    deque<Version*>* version_list = mvcc_data_[key];

    // Find the correct position to insert the new version
    auto it = version_list->begin();
    while (it != version_list->end() && (*it)->version_id_ > txn_unique_id)
    {
        ++it;
    }

    // Insert the new version at the correct position
    version_list->insert(it, new_version);

}
