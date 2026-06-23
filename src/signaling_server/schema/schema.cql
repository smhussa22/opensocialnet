-- ScyllaDB schema for the signaling server.
-- Apply with: cqlsh -f schema.cql  (against localhost:9042 via docker)

CREATE KEYSPACE IF NOT EXISTS opensocialnet
  WITH REPLICATION = {
    'class': 'SimpleStrategy',
    'replication_factor': 1
  };

USE opensocialnet;


-- Messages: the hot path.
--
-- Partition key (channel_id) -> all messages for one channel live on
-- the same replica set, so a single read serves a history page.
--
-- Clustering key (message_id DESC) -> rows are stored newest-first
-- on disk, so "give me the last N messages" is a sequential read
-- with no in-memory sort.
--
-- message_id is a TimeUUID: 128 bits encoding (timestamp + random),
-- monotonic per node, and globally unique without a coordinator.
-- toTimestamp(message_id) recovers the wall-clock time, so we don't
-- store a separate timestamp column.
CREATE TABLE IF NOT EXISTS messages (
  channel_id  text,
  message_id  timeuuid,
  sender_id   text,
  content     text,
  PRIMARY KEY ((channel_id), message_id)
) WITH CLUSTERING ORDER BY (message_id DESC);


-- Channels: metadata. Partition key is the channel itself; this table
-- is read-mostly and small.
CREATE TABLE IF NOT EXISTS channels (
  channel_id  text PRIMARY KEY,
  guild_id    text,
  name        text,
  kind        text  -- 'text' or 'voice'
);


-- Users: minimal profile. Real auth (password hashes, sessions, etc.)
-- belongs in a separate auth service / table -- keep this lean.
CREATE TABLE IF NOT EXISTS users (
  user_id     text PRIMARY KEY,
  username    text,
  email       text,
  created_at  timestamp
);


-- Reverse index: email -> user_id, for friend lookup by email. Scylla
-- doesn't do JOINs and SIs are awkward at scale, so we denormalize on
-- write (every upsert_user writes both tables).
CREATE TABLE IF NOT EXISTS users_by_email (
  email       text PRIMARY KEY,
  user_id     text,
  username    text
);


-- Channel membership: which users belong to which channels. Used by
-- the gateway on Hello to determine which Kafka partitions to consume
-- and which channels to include in the Ready response.
--
-- Partitioned by channel_id so the gateway can list members of a
-- channel in one read (e.g. when broadcasting VoicePeerJoined).
CREATE TABLE IF NOT EXISTS channel_members (
  channel_id  text,
  user_id     text,
  joined_at   timestamp,
  PRIMARY KEY ((channel_id), user_id)
);


-- Reverse index: given a user, what channels are they in?
-- Cassandra/Scylla doesn't do JOINs -- you denormalize. We write
-- to both channel_members and user_channels on every join/leave.
CREATE TABLE IF NOT EXISTS user_channels (
  user_id     text,
  channel_id  text,
  joined_at   timestamp,
  PRIMARY KEY ((user_id), channel_id)
);


-- ============================================================
-- Friends / DMs
-- ============================================================

-- Pending friend requests, partitioned by recipient so "show me my
-- incoming requests" is one read. Row gets deleted when accepted
-- (and a friendships row appears) or when rejected.
CREATE TABLE IF NOT EXISTS friend_requests (
  to_user_id    text,
  from_user_id  text,
  created_at    timestamp,
  PRIMARY KEY ((to_user_id), from_user_id)
);


-- Bidirectional friendships. Each pair gets two rows so lookup-by-
-- either-side is one partition read with no secondary index.
-- dm_channel_id is the shared 1-on-1 channel that gets created on
-- accept; same channel_id appears in both rows of the pair.
CREATE TABLE IF NOT EXISTS friendships (
  user_id         text,
  friend_user_id  text,
  dm_channel_id   text,
  since           timestamp,
  PRIMARY KEY ((user_id), friend_user_id)
);
