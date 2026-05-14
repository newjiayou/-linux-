-- 仅用于历史消息查询加速：会话ID + 时间戳联合索引
ALTER TABLE all_messages_log
  ADD INDEX idx_session_created_at (session_id, created_at);
