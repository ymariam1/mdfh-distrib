#!/usr/bin/env python3
"""
Simple test client for the market feed simulator.
Connects to the simulator and prints received messages.
"""

import socket
import struct
import time
import argparse

def parse_message(data):
    """Parse a 20-byte message from the feed simulator."""
    if len(data) < 20:
        return None
    
    # Unpack: uint64_t seq, double px, int32_t qty
    seq, px, qty = struct.unpack('<Qdi', data[:20])
    return {
        'seq': seq,
        'price': px,
        'quantity': qty,
        'side': 'BUY' if qty > 0 else 'SELL'
    }

def main():
    parser = argparse.ArgumentParser(description='Test client for market feed simulator')
    parser.add_argument('--host', default='localhost', help='Simulator host')
    parser.add_argument('--port', type=int, default=9001, help='Simulator port')
    parser.add_argument('--count', type=int, default=100, help='Number of messages to receive')
    
    args = parser.parse_args()
    
    print(f"Connecting to feed simulator at {args.host}:{args.port}")
    
    try:
        # Connect to the simulator
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((args.host, args.port))
        
        print(f"Connected! Receiving {args.count} messages...")
        print("SEQ        PRICE       QTY    SIDE")
        print("-" * 40)
        
        buffer = b''
        msg_count = 0
        
        while msg_count < args.count:
            # Receive data
            data = sock.recv(4096)
            if not data:
                print("Connection closed by server")
                break
                
            buffer += data
            
            # Process complete messages
            while len(buffer) >= 20:
                msg = parse_message(buffer[:20])
                if msg:
                    print(f"{msg['seq']:8d} {msg['price']:10.4f} {abs(msg['quantity']):6d} {msg['side']:4s}")
                    msg_count += 1
                    
                    if msg_count >= args.count:
                        break
                        
                buffer = buffer[20:]
                
    except KeyboardInterrupt:
        print("\nInterrupted by user")
    except Exception as e:
        print(f"Error: {e}")
    finally:
        try:
            sock.close()
        except:
            pass

if __name__ == '__main__':
    main() 