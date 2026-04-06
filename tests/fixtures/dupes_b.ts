import { db } from '../database';

export async function getUserDetails(uid: string) {
  const user = await db.users.findById(uid);
  if (!user) {
    throw new Error('User not found');
  }

  const details = {
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

  return details;
}

export function processApiResponse(payload: unknown) {
  const response = {
    success: true,
    data: payload,
    timestamp: new Date().toISOString(),
    version: 'v1',
  };
  return response;
}
