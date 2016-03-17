# SQLite LSM port for Windows

## Intorduction
LSM is an embedded database library for key-value data, roughly similar in scope to [Berkeley DB](http://www.oracle.com/technetwork/products/berkeleydb/overview/index.html), [LevelDB](http://code.google.com/p/leveldb/) or [KyotoCabinet](http://fallabs.com/kyotocabinet/). Both keys and values are specified and stored as byte arrays. Duplicate keys are not supported. Keys are always sorted in memcmp() order. LSM supports the following operations for the manipulation and query of database data:

*   Writing a new key and value into the database.
*   Deleting an existing key from the database.
*   Deleting a range of keys from the database.
*   Querying the database for a specific key.
*   Iterating through a range of database keys (either forwards or backwards).

Other salient features are:

*   A **single-writer/multiple-reader MVCC** based transactional concurrency model. SQL style nested sub-transactions are supported. Clients may concurrently access a single LSM database from within a single process or multiple application processes.

*   An entire database is stored in a **single file on disk**.

*   Data **durability in the face of application or power failure**. LSM may optionally use a write-ahead log file when writing to the database to ensure committed transactions are not lost if an application or power failure occurs.

*   An API that **allows external data compression and/or encryption routines to be used** to create and access compressed and/or encrypted databases.

Many database systems that support range queries, including [SQLite 3](http://www.sqlite.org), Berkeley DB and Kyoto Cabinet, are based on one of many variants of the [b-tree data structure](http://en.wikipedia.org/wiki/B-tree). B-trees are attractive because a b-tree structure minimizes the number of disk sectors that must be read from disk when searching the database for a specific key. However, b-tree implementations usually suffer from poor write localization - updating the contents of a b-tree often involves modifying the contents of nodes scattered throughout the database file. If the database is stored on a spinning disk (HDD), then the disk heads must be moved between writing non-contiguous sectors, which is extremely slow. If the database is stored on solid state storage (SDD) a similar phenomena is encountered due to the large erase-block sizes. In general, writing to a series of contiguous disk sectors is orders of magnitude faster than updating to the same number of disk sectors scattered randomly throughout a large file. Additionally, b-tree structures are prone to fragmentation, reducing the speed of range queries.

## What is this repository then?

As most of the readers might be already aware of the fact that LSM right now is not ported to Win32/WinRT APIs. This respository aims to solve that problem. This project would allow you to compile binary DLLs for Windows.

## User Manual

Checkout [SQLite4: LSM User Manual](https://www.sqlite.org/src4/doc/trunk/www/lsmusr.wiki)
