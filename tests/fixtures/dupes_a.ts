import { db } from '../database';

export async function fetchUserProfile(userId: string) {
  const user = await db.users.findById(userId);
  if (!user) {
    throw new Error('User not found');
  }

  const profile = {
    id: user.id,
    name: user.name,
    email: user.email,
    avatar: user.avatar || 'default.png',
    createdAt: user.createdAt.toISOString(),
    updatedAt: user.updatedAt.toISOString(),
    isActive: user.status === 'active',
    role: user.role || 'user',
    lastLogin: user.lastLogin ? user.lastLogin.toISOString() : null,
  };

  return profile;
}

export function formatUserResponse(data: any) {
  const result = {
    success: true,
    data: data,
    timestamp: new Date().toISOString(),
    version: 'v1',
  };
  return result as any;
}

export function parseConfig(raw: any) {
  const config = raw as any;
  // @ts-ignore
  const port = config.port || 3000;
  const host = config.host as any;
  return { port, host };
}
