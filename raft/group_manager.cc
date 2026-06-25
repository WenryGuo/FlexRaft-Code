RCF::ByteBuffer RaftRPCService::AppendEntries(const RCF::ByteBuffer &arg_buf) {
  AppendEntriesArgs args;
  AppendEntriesReply reply;

  auto serializer = Serializer::NewSerializer();
  serializer.Deserialize(&arg_buf, &args);

  // 注意：之前这里是 raft_->Process(&args, &reply);
  // 改为把 RPC 的目标从 void* state 强转为 RaftManager*
  if (raft_manager_ != nullptr) {
    raft_manager_->Process(args.group_id, &args, &reply);
  } else if (raft_ != nullptr) {
    // 向后兼容：如果还没有改造 manager，仍旧调用旧的 raft_
    raft_->Process(&args, &reply);
  } else {
    // fallback: 返回空回复或按照原来逻辑返回 chunk infos
    reply.success = false;
  }

  // 序列化 reply 并返回
  RCF::ByteBuffer out_buf;
  serializer.Serialize(&reply, &out_buf);
  return out_buf;
}