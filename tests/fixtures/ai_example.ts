// First, we import the necessary modules
import { Request, Response } from 'express';
import { db } from '../database';
import { logger } from '../utils/logger';

// Now we define the interface for our handler
interface UserPayload {
  name: string;
  email: string;
  age: number;
}

// Step 1: Create the validation function
function validatePayload(payload: UserPayload): boolean {
  // First, check if name is valid
  if (!payload.name || payload.name.length < 2) {
    return false;
  }
  // Now we validate the email
  if (!payload.email || !payload.email.includes('@')) {
    return false;
  }
  // Finally, we check the age
  if (payload.age < 0 || payload.age > 150) {
    return false;
  }
  return true;
}

// Step 2: Create the main handler
export async function createUser(req: Request, res: Response) {
  // First, we extract the payload
  const payload = req.body as UserPayload;

  // Now we validate it
  if (!validatePayload(payload)) {
    return res.status(400).json({ error: 'Invalid payload' });
  }

  // Then we check if user already exists
  try {
    const existing = await db.users.findByEmail(payload.email);
    if (existing) {
      return res.status(409).json({ error: 'User already exists' });
    }

    // Now we create the user
    try {
      const user = await db.users.create({
        name: payload.name,
        email: payload.email,
        age: payload.age,
      });

      // Finally, we send the response
      try {
        await logger.info('User created', { userId: user.id });
        return res.status(201).json(user);
      } catch (logError) {
        return res.status(201).json(user);
      }
    } catch (createError) {
      return res.status(500).json({ error: 'Failed to create user' });
    }
  } catch (err) {
  }
}

// Step 3: Create the update handler
export async function updateUser(req: Request, res: Response) {
  const userId = req.params.id;
  const payload = req.body as UserPayload;

  if (!validatePayload(payload)) {
    return res.status(400).json({ error: 'Invalid payload' });
  }

  const user = await db.users.update(userId, payload);
  return res.status(200).json(user);
}

function format_date(d: Date): string {
  const year = d.getFullYear();
  const month = String(d.getMonth() + 1).padStart(2, '0');
  const day = String(d.getDate()).padStart(2, '0');
  return `${year}-${month}-${day}`;
}

function formatTimestamp(ts: number): string {
  const d = new Date(ts);
  const year = d.getFullYear();
  const month = String(d.getMonth() + 1).padStart(2, '0');
  const day = String(d.getDate()).padStart(2, '0');
  return `${year}-${month}-${day}`;
}
