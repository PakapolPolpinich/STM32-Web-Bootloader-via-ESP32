with open('test2led.bin', 'rb') as f:
        bin_data = f.read()

print(len(bin_data))
# for i in range(0, len(bin_data), 256): #increase per 256
#     chunk = bin_data[i:i+256]
#     #print(chunk)
#     n = len(chunk) - 1
#     block = bytes([n]) + chunk
#     print(block)
#     print("               ")