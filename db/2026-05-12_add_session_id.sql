-- 新增会话ID：由 sender/target 规范化后 MD5 生成
-- 规范化规则：LEAST(sender,target) + '#' + GREATEST(sender,target)

ALTER TABLE all_messages_log
  ADD COLUMN session_id CHAR(32) NOT NULL DEFAULT '' AFTER id;

-- 回填历史数据
UPDATE all_messages_log
SET session_id = MD5(CONCAT(LEAST(sender, target), '#', GREATEST(sender, target)))
WHERE session_id = '' OR session_id IS NULL;

-- 历史查询核心索引：按会话 + 时间
ALTER TABLE all_messages_log
  ADD INDEX idx_session_created_at (session_id, created_at);

-- 可选：若你不再依赖旧条件查询，可后续再考虑删除旧索引
-- ALTER TABLE all_messages_log DROP INDEX idx_target_created_at;
-- ALTER TABLE all_messages_log DROP INDEX idx_sender_created_at;
